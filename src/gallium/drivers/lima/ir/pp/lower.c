/*
 * Copyright (c) 2017 Lima Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

#include "util/bitscan.h"
#include "util/ralloc.h"

#include "ppir.h"

static bool ppir_lower_const(ppir_block *block, ppir_node *node)
{
   if (ppir_node_is_root(node)) {
      ppir_node_delete(node);
      return true;
   }

   ppir_node *move = NULL;
   ppir_dest *dest = ppir_node_get_dest(node);

   /* const (register) can only be used in alu node, create a move
    * node for other types of node */
   ppir_node_foreach_succ_safe(node, dep) {
      ppir_node *succ = dep->succ;

      if (succ->type != ppir_node_type_alu) {
         if (!move) {
            move = ppir_node_create(block, ppir_op_mov, -1, 0);
            if (unlikely(!move))
               return false;

            ppir_debug("lower const create move %d for %d\n",
                       move->index, node->index);

            ppir_alu_node *alu = ppir_node_to_alu(move);
            alu->dest = *dest;
            alu->num_src = 1;
            ppir_node_target_assign(alu->src, dest);
            for (int i = 0; i < 4; i++)
               alu->src->swizzle[i] = i;
         }

         ppir_node_replace_pred(dep, move);
         ppir_node_replace_child(succ, node, move);
      }
   }

   if (move) {
      ppir_node_add_dep(move, node);
      list_addtail(&move->list, &node->list);
   }

   return true;
}

static ppir_reg *create_reg(ppir_compiler *comp, int num_components)
{
   ppir_reg *r = rzalloc(comp, ppir_reg);
   if (!r)
      return NULL;

   r->num_components = num_components;
   r->live_in = INT_MAX;
   r->live_out = 0;
   r->is_head = false;
   list_addtail(&r->list, &comp->reg_list);

   return r;
}

/* lower vector alu node to multi scalar nodes */
static bool ppir_lower_vec_to_scalar(ppir_block *block, ppir_node *node)
{
   ppir_alu_node *alu = ppir_node_to_alu(node);
   ppir_dest *dest = &alu->dest;

   int n = 0;
   int index[4];

   unsigned mask = dest->write_mask;
   while (mask)
      index[n++] = u_bit_scan(&mask);

   if (n == 1)
      return true;

   ppir_reg *r;
   /* we need a reg for scalar nodes to store output */
   if (dest->type == ppir_target_register)
      r = dest->reg;
   else {
      r = create_reg(block->comp, n);
      if (!r)
         return false;

      /* change all successors to use reg r */
      ppir_node_foreach_succ(node, dep) {
         ppir_node *succ = dep->succ;
         if (succ->type == ppir_node_type_alu) {
            ppir_alu_node *sa = ppir_node_to_alu(succ);
            for (int i = 0; i < sa->num_src; i++) {
               ppir_src *src = sa->src + i;
               if (ppir_node_target_equal(src, dest)) {
                  src->type = ppir_target_register;
                  src->reg = r;
               }
            }
         }
         else {
            assert(succ->type == ppir_node_type_store);
            ppir_store_node *ss = ppir_node_to_store(succ);
            ppir_src *src = &ss->src;
            src->type = ppir_target_register;
            src->reg = r;
         }
      }
   }

   /* create each component's scalar node */
   for (int i = 0; i < n; i++) {
      ppir_node *s = ppir_node_create(block, node->op, -1, 0);
      if (!s)
         return false;
      list_addtail(&s->list, &node->list);

      ppir_alu_node *sa = ppir_node_to_alu(s);
      ppir_dest *sd = &sa->dest;
      sd->type = ppir_target_register;
      sd->reg = r;
      sd->modifier = dest->modifier;
      sd->write_mask = 1 << index[i];

      for (int j = 0; j < alu->num_src; j++)
         sa->src[j] = alu->src[j];
      sa->num_src = alu->num_src;

      /* TODO: need per reg component dependancy */
      ppir_node_foreach_succ(node, dep) {
         ppir_node_add_dep(dep->succ, s);
      }

      ppir_node_foreach_pred(node, dep) {
         ppir_node_add_dep(s, dep->pred);
      }
   }

   ppir_node_delete(node);
   return true;
}

static bool ppir_lower_swap_args(ppir_block *block, ppir_node *node)
{
   /* swapped op must be the next op */
   node->op++;

   assert(node->type == ppir_node_type_alu);
   ppir_alu_node *alu = ppir_node_to_alu(node);
   assert(alu->num_src == 2);

   ppir_src tmp = alu->src[0];
   alu->src[0] = alu->src[1];
   alu->src[1] = tmp;
   return true;
}

static bool ppir_lower_texture(ppir_block *block, ppir_node *node)
{
   ppir_load_texture_node *load_tex = ppir_node_to_load_texture(node);

   /* Create load_coords node */
   ppir_load_node *load = ppir_node_create(block, ppir_op_load_coords, -1, 0);
   if (!load)
      return false;
   list_addtail(&load->node.list, &node->list);

   ppir_debug("%s create load_coords node %d for %d\n",
              __FUNCTION__, load->node.index, node->index);

   load->dest.type = ppir_target_pipeline;
   load->dest.pipeline = ppir_pipeline_reg_discard;

   load->src = load_tex->src_coords;

   ppir_node_foreach_pred_safe(node, dep) {
      ppir_node *pred = dep->pred;
      ppir_node_remove_dep(dep);
      ppir_node_add_dep(&load->node, pred);
   }

   ppir_node_add_dep(node, &load->node);
   return true;
}

/* Prepare for sin and cos and then lower vector alu node to multi
 * scalar nodes */
static bool ppir_lower_sin_cos_vec_to_scalar(ppir_block *block, ppir_node *node)
{
   ppir_alu_node *alu = ppir_node_to_alu(node);

   ppir_node *inv_2pi_node = ppir_node_create(block, ppir_op_const, -1, 0);
   if (!inv_2pi_node)
      return false;
   list_addtail(&inv_2pi_node->list, &node->list);

   /* For sin and cos, the input has to multiplied by the constant
    * 1/(2*pi), presumably to simplify the hardware. */
   ppir_const_node *inv_2pi_const = ppir_node_to_const(inv_2pi_node);
   inv_2pi_const->constant.num = 1;
   inv_2pi_const->constant.value[0].f = (1.0f/(2.0f * M_PI));

   inv_2pi_const->dest.type = ppir_target_ssa;
   inv_2pi_const->dest.ssa.num_components = 1;
   inv_2pi_const->dest.ssa.live_in = INT_MAX;
   inv_2pi_const->dest.ssa.live_out = 0;
   inv_2pi_const->dest.write_mask = 0x01;

   ppir_node *mul_node = ppir_node_create(block, ppir_op_mul, -1, 0);
   if (!mul_node)
      return false;
   list_addtail(&mul_node->list, &node->list);

   ppir_alu_node *mul_alu = ppir_node_to_alu(mul_node);
   mul_alu->num_src = 2;
   mul_alu->src[0] = alu->src[0];
   mul_alu->src[1].type = ppir_target_ssa;
   mul_alu->src[1].ssa = &inv_2pi_const->dest.ssa;

   int num_components = alu->src[0].ssa->num_components;
   mul_alu->dest.type = ppir_target_ssa;
   mul_alu->dest.ssa.num_components = num_components;
   mul_alu->dest.ssa.live_in = INT_MAX;
   mul_alu->dest.ssa.live_out = 0;
   mul_alu->dest.write_mask = u_bit_consecutive(0, num_components);

   alu->src[0].type = ppir_target_ssa;
   alu->src[0].ssa = &mul_alu->dest.ssa;
   for (int i = 0; i < 4; i++)
      alu->src->swizzle[i] = i;

   ppir_node_foreach_pred_safe(node, dep) {
      ppir_node *pred = dep->pred;
      ppir_node_remove_dep(dep);
      ppir_node_add_dep(mul_node, pred);
   }
   ppir_node_add_dep(node, mul_node);
   ppir_node_add_dep(mul_node, inv_2pi_node);

   return ppir_lower_vec_to_scalar(block, node);
}

/* insert a move as the select condition to make sure it can
 * be inserted to select instr float mul slot
 */
static bool ppir_lower_select(ppir_block *block, ppir_node *node)
{
   ppir_alu_node *alu = ppir_node_to_alu(node);

   ppir_node *move = ppir_node_create(block, ppir_op_mov, -1, 0);
   if (!move)
      return false;
   list_addtail(&move->list, &node->list);

   ppir_alu_node *move_alu = ppir_node_to_alu(move);
   ppir_src *move_src = move_alu->src, *src = alu->src;
   move_src->type = src->type;
   move_src->ssa = src->ssa;
   move_src->swizzle[0] = src->swizzle[0];
   move_alu->num_src = 1;

   ppir_dest *move_dest = &move_alu->dest;
   move_dest->type = ppir_target_ssa;
   move_dest->ssa.num_components = 1;
   move_dest->ssa.live_in = INT_MAX;
   move_dest->ssa.live_out = 0;
   move_dest->write_mask = 1;

   ppir_node_foreach_pred(node, dep) {
      ppir_node *pred = dep->pred;
      ppir_dest *dest = ppir_node_get_dest(pred);
      if (ppir_node_target_equal(alu->src, dest)) {
         ppir_node_replace_pred(dep, move);
         ppir_node_add_dep(move, pred);
      }
   }

   /* move must be the first pred of select node which make sure
    * the float mul slot is free when node to instr
    */
   assert(ppir_node_first_pred(node) == move);

   src->swizzle[0] = 0;
   ppir_node_target_assign(alu->src, move_dest);
   return true;
}

static bool ppir_lower_trunc(ppir_block *block, ppir_node *node)
{
   /* Turn it into a mov with a round to integer output modifier */
   ppir_alu_node *alu = ppir_node_to_alu(node);
   ppir_dest *move_dest = &alu->dest;
   move_dest->modifier = ppir_outmod_round;
   node->op = ppir_op_mov;

   return true;
}

static bool ppir_lower_abs(ppir_block *block, ppir_node *node)
{
   /* Turn it into a mov and set the absolute modifier */
   ppir_alu_node *alu = ppir_node_to_alu(node);

   assert(alu->num_src == 1);

   alu->src[0].absolute = true;
   alu->src[0].negate = false;
   node->op = ppir_op_mov;

   return true;
}

static bool ppir_lower_neg(ppir_block *block, ppir_node *node)
{
   /* Turn it into a mov and set the negate modifier */
   ppir_alu_node *alu = ppir_node_to_alu(node);

   assert(alu->num_src == 1);

   alu->src[0].negate = !alu->src[0].negate;
   node->op = ppir_op_mov;

   return true;
}

static bool ppir_lower_sat(ppir_block *block, ppir_node *node)
{
   /* Turn it into a mov with the saturate output modifier */
   ppir_alu_node *alu = ppir_node_to_alu(node);

   assert(alu->num_src == 1);

   ppir_dest *move_dest = &alu->dest;
   move_dest->modifier = ppir_outmod_clamp_fraction;
   node->op = ppir_op_mov;

   return true;
}

static bool ppir_lower_branch(ppir_block *block, ppir_node *node)
{
   ppir_branch_node *branch = ppir_node_to_branch(node);
   ppir_const_node *zero = ppir_node_create(block, ppir_op_const, -1, 0);

   if (!zero)
      return false;

   list_addtail(&zero->node.list, &node->list);

   zero->constant.value[0].f = 0;
   zero->constant.num = 1;
   zero->dest.type = ppir_target_ssa;
   zero->dest.ssa.num_components = 1;
   zero->dest.ssa.live_in = INT_MAX;
   zero->dest.ssa.live_out = 0;
   zero->dest.write_mask = 0x01;

   /* For now we're just comparing branch condition with 0,
    * in future we should look whether it's possible to move
    * comparision node into branch itself and use current
    * way as a fallback for complex conditions.
    */
   branch->src[1].type = ppir_target_ssa;
   branch->src[1].ssa = &zero->dest.ssa;

   branch->cond_gt = true;
   branch->cond_lt = true;

   ppir_node_add_dep(&branch->node, &zero->node);

   return true;
}

static bool (*ppir_lower_funcs[ppir_op_num])(ppir_block *, ppir_node *) = {
   [ppir_op_abs] = ppir_lower_abs,
   [ppir_op_neg] = ppir_lower_neg,
   [ppir_op_const] = ppir_lower_const,
   [ppir_op_rcp] = ppir_lower_vec_to_scalar,
   [ppir_op_rsqrt] = ppir_lower_vec_to_scalar,
   [ppir_op_log2] = ppir_lower_vec_to_scalar,
   [ppir_op_exp2] = ppir_lower_vec_to_scalar,
   [ppir_op_sqrt] = ppir_lower_vec_to_scalar,
   [ppir_op_sin] = ppir_lower_sin_cos_vec_to_scalar,
   [ppir_op_cos] = ppir_lower_sin_cos_vec_to_scalar,
   [ppir_op_lt] = ppir_lower_swap_args,
   [ppir_op_le] = ppir_lower_swap_args,
   [ppir_op_load_texture] = ppir_lower_texture,
   [ppir_op_select] = ppir_lower_select,
   [ppir_op_trunc] = ppir_lower_trunc,
   [ppir_op_sat] = ppir_lower_sat,
   [ppir_op_branch] = ppir_lower_branch,
};

bool ppir_lower_prog(ppir_compiler *comp)
{
   list_for_each_entry(ppir_block, block, &comp->block_list, list) {
      list_for_each_entry_safe(ppir_node, node, &block->node_list, list) {
         if (ppir_lower_funcs[node->op] &&
             !ppir_lower_funcs[node->op](block, node))
            return false;
      }
   }

   ppir_node_print_prog(comp);
   return true;
}
