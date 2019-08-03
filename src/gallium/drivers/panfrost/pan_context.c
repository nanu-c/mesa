/*
 * © Copyright 2018 Alyssa Rosenzweig
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#include <sys/poll.h>
#include <errno.h>

#include "pan_context.h"
#include "pan_format.h"

#include "util/macros.h"
#include "util/u_format.h"
#include "util/u_inlines.h"
#include "util/u_upload_mgr.h"
#include "util/u_memory.h"
#include "util/u_vbuf.h"
#include "util/half_float.h"
#include "util/u_helpers.h"
#include "util/u_format.h"
#include "util/u_prim_restart.h"
#include "indices/u_primconvert.h"
#include "tgsi/tgsi_parse.h"
#include "tgsi/tgsi_from_mesa.h"
#include "util/u_math.h"

#include "pan_screen.h"
#include "pan_blending.h"
#include "pan_blend_shaders.h"
#include "pan_util.h"
#include "pan_tiler.h"

/* Do not actually send anything to the GPU; merely generate the cmdstream as fast as possible. Disables framebuffer writes */
//#define DRY_RUN

/* Framebuffer descriptor */

static struct midgard_tiler_descriptor
panfrost_emit_midg_tiler(
        struct panfrost_context *ctx,
        unsigned width,
        unsigned height,
        unsigned vertex_count)
{
        struct midgard_tiler_descriptor t = {};
        struct panfrost_job *batch = panfrost_get_job_for_fbo(ctx);

        t.hierarchy_mask =
                panfrost_choose_hierarchy_mask(width, height, vertex_count);

        /* Compute the polygon header size and use that to offset the body */

        unsigned header_size = panfrost_tiler_header_size(
                                       width, height, t.hierarchy_mask);

        unsigned body_size = panfrost_tiler_body_size(
                                     width, height, t.hierarchy_mask);

        /* Sanity check */

        if (t.hierarchy_mask) {
                t.polygon_list = panfrost_job_get_polygon_list(batch, header_size + body_size);

                /* Allow the entire tiler heap */
                t.heap_start = ctx->tiler_heap.bo->gpu;
                t.heap_end =
                        ctx->tiler_heap.bo->gpu + ctx->tiler_heap.bo->size;
        } else {
                /* The tiler is disabled, so don't allow the tiler heap */
                t.heap_start = ctx->tiler_heap.bo->gpu;
                t.heap_end = t.heap_start;

                /* Use a dummy polygon list */
                t.polygon_list = ctx->tiler_dummy.bo->gpu;

                /* Also, set a "tiler disabled?" flag? */
                t.hierarchy_mask |= 0x1000;
        }

        t.polygon_list_body =
                t.polygon_list + header_size;

        t.polygon_list_size =
                header_size + body_size;

        return t;
}

struct mali_single_framebuffer
panfrost_emit_sfbd(struct panfrost_context *ctx, unsigned vertex_count)
{
        unsigned width = ctx->pipe_framebuffer.width;
        unsigned height = ctx->pipe_framebuffer.height;

        struct mali_single_framebuffer framebuffer = {
                .width = MALI_POSITIVE(width),
                .height = MALI_POSITIVE(height),
                .unknown2 = 0x1f,
                .format = 0x30000000,
                .clear_flags = 0x1000,
                .unknown_address_0 = ctx->scratchpad.bo->gpu,
                .tiler = panfrost_emit_midg_tiler(ctx,
                                                  width, height, vertex_count),
        };

        return framebuffer;
}

struct bifrost_framebuffer
panfrost_emit_mfbd(struct panfrost_context *ctx, unsigned vertex_count)
{
        unsigned width = ctx->pipe_framebuffer.width;
        unsigned height = ctx->pipe_framebuffer.height;

        struct bifrost_framebuffer framebuffer = {
                .unk0 = 0x1e5, /* 1e4 if no spill */
                .width1 = MALI_POSITIVE(width),
                .height1 = MALI_POSITIVE(height),
                .width2 = MALI_POSITIVE(width),
                .height2 = MALI_POSITIVE(height),

                .unk1 = 0x1080,

                .rt_count_1 = MALI_POSITIVE(ctx->pipe_framebuffer.nr_cbufs),
                .rt_count_2 = 4,

                .unknown2 = 0x1f,

                .scratchpad = ctx->scratchpad.bo->gpu,
                .tiler = panfrost_emit_midg_tiler(ctx,
                                                  width, height, vertex_count)
        };

        return framebuffer;
}

/* Are we currently rendering to the screen (rather than an FBO)? */

bool
panfrost_is_scanout(struct panfrost_context *ctx)
{
        /* If there is no color buffer, it's an FBO */
        if (ctx->pipe_framebuffer.nr_cbufs != 1)
                return false;

        /* If we're too early that no framebuffer was sent, it's scanout */
        if (!ctx->pipe_framebuffer.cbufs[0])
                return true;

        return ctx->pipe_framebuffer.cbufs[0]->texture->bind & PIPE_BIND_DISPLAY_TARGET ||
               ctx->pipe_framebuffer.cbufs[0]->texture->bind & PIPE_BIND_SCANOUT ||
               ctx->pipe_framebuffer.cbufs[0]->texture->bind & PIPE_BIND_SHARED;
}

static void
panfrost_clear(
        struct pipe_context *pipe,
        unsigned buffers,
        const union pipe_color_union *color,
        double depth, unsigned stencil)
{
        struct panfrost_context *ctx = pan_context(pipe);
        struct panfrost_job *job = panfrost_get_job_for_fbo(ctx);

        panfrost_job_clear(ctx, job, buffers, color, depth, stencil);
}

static mali_ptr
panfrost_attach_vt_mfbd(struct panfrost_context *ctx)
{
        struct bifrost_framebuffer mfbd = panfrost_emit_mfbd(ctx, ~0);

        return panfrost_upload_transient(ctx, &mfbd, sizeof(mfbd)) | MALI_MFBD;
}

static mali_ptr
panfrost_attach_vt_sfbd(struct panfrost_context *ctx)
{
        struct mali_single_framebuffer sfbd = panfrost_emit_sfbd(ctx, ~0);

        return panfrost_upload_transient(ctx, &sfbd, sizeof(sfbd)) | MALI_SFBD;
}

static void
panfrost_attach_vt_framebuffer(struct panfrost_context *ctx)
{
        /* Skip the attach if we can */

        if (ctx->payloads[PIPE_SHADER_VERTEX].postfix.framebuffer) {
                assert(ctx->payloads[PIPE_SHADER_FRAGMENT].postfix.framebuffer);
                return;
        }

        struct panfrost_screen *screen = pan_screen(ctx->base.screen);
        mali_ptr framebuffer = screen->require_sfbd ?
                               panfrost_attach_vt_sfbd(ctx) :
                               panfrost_attach_vt_mfbd(ctx);

        for (unsigned i = 0; i < PIPE_SHADER_TYPES; ++i)
                ctx->payloads[i].postfix.framebuffer = framebuffer;
}

/* Reset per-frame context, called on context initialisation as well as after
 * flushing a frame */

static void
panfrost_invalidate_frame(struct panfrost_context *ctx)
{
        for (unsigned i = 0; i < PIPE_SHADER_TYPES; ++i)
                ctx->payloads[i].postfix.framebuffer = 0;

        if (ctx->rasterizer)
                ctx->dirty |= PAN_DIRTY_RASTERIZER;

        /* XXX */
        ctx->dirty |= PAN_DIRTY_SAMPLERS | PAN_DIRTY_TEXTURES;
}

/* In practice, every field of these payloads should be configurable
 * arbitrarily, which means these functions are basically catch-all's for
 * as-of-yet unwavering unknowns */

static void
panfrost_emit_vertex_payload(struct panfrost_context *ctx)
{
        /* 0x2 bit clear on 32-bit T6XX */

        struct midgard_payload_vertex_tiler payload = {
                .gl_enables = 0x4 | 0x2,
        };

        /* Vertex and compute are closely coupled, so share a payload */

        memcpy(&ctx->payloads[PIPE_SHADER_VERTEX], &payload, sizeof(payload));
        memcpy(&ctx->payloads[PIPE_SHADER_COMPUTE], &payload, sizeof(payload));
}

static void
panfrost_emit_tiler_payload(struct panfrost_context *ctx)
{
        struct midgard_payload_vertex_tiler payload = {
                .prefix = {
                        .zero1 = 0xffff, /* Why is this only seen on test-quad-textured? */
                },
        };

        memcpy(&ctx->payloads[PIPE_SHADER_FRAGMENT], &payload, sizeof(payload));
}

static unsigned
translate_tex_wrap(enum pipe_tex_wrap w)
{
        switch (w) {
        case PIPE_TEX_WRAP_REPEAT:
                return MALI_WRAP_REPEAT;

        case PIPE_TEX_WRAP_CLAMP_TO_EDGE:
                return MALI_WRAP_CLAMP_TO_EDGE;

        case PIPE_TEX_WRAP_CLAMP_TO_BORDER:
                return MALI_WRAP_CLAMP_TO_BORDER;

        case PIPE_TEX_WRAP_MIRROR_REPEAT:
                return MALI_WRAP_MIRRORED_REPEAT;

        default:
                unreachable("Invalid wrap");
        }
}

static unsigned
panfrost_translate_compare_func(enum pipe_compare_func in)
{
        switch (in) {
        case PIPE_FUNC_NEVER:
                return MALI_FUNC_NEVER;

        case PIPE_FUNC_LESS:
                return MALI_FUNC_LESS;

        case PIPE_FUNC_EQUAL:
                return MALI_FUNC_EQUAL;

        case PIPE_FUNC_LEQUAL:
                return MALI_FUNC_LEQUAL;

        case PIPE_FUNC_GREATER:
                return MALI_FUNC_GREATER;

        case PIPE_FUNC_NOTEQUAL:
                return MALI_FUNC_NOTEQUAL;

        case PIPE_FUNC_GEQUAL:
                return MALI_FUNC_GEQUAL;

        case PIPE_FUNC_ALWAYS:
                return MALI_FUNC_ALWAYS;

        default:
                unreachable("Invalid func");
        }
}

static unsigned
panfrost_translate_alt_compare_func(enum pipe_compare_func in)
{
        switch (in) {
        case PIPE_FUNC_NEVER:
                return MALI_ALT_FUNC_NEVER;

        case PIPE_FUNC_LESS:
                return MALI_ALT_FUNC_LESS;

        case PIPE_FUNC_EQUAL:
                return MALI_ALT_FUNC_EQUAL;

        case PIPE_FUNC_LEQUAL:
                return MALI_ALT_FUNC_LEQUAL;

        case PIPE_FUNC_GREATER:
                return MALI_ALT_FUNC_GREATER;

        case PIPE_FUNC_NOTEQUAL:
                return MALI_ALT_FUNC_NOTEQUAL;

        case PIPE_FUNC_GEQUAL:
                return MALI_ALT_FUNC_GEQUAL;

        case PIPE_FUNC_ALWAYS:
                return MALI_ALT_FUNC_ALWAYS;

        default:
                unreachable("Invalid alt func");
        }
}

static unsigned
panfrost_translate_stencil_op(enum pipe_stencil_op in)
{
        switch (in) {
        case PIPE_STENCIL_OP_KEEP:
                return MALI_STENCIL_KEEP;

        case PIPE_STENCIL_OP_ZERO:
                return MALI_STENCIL_ZERO;

        case PIPE_STENCIL_OP_REPLACE:
                return MALI_STENCIL_REPLACE;

        case PIPE_STENCIL_OP_INCR:
                return MALI_STENCIL_INCR;

        case PIPE_STENCIL_OP_DECR:
                return MALI_STENCIL_DECR;

        case PIPE_STENCIL_OP_INCR_WRAP:
                return MALI_STENCIL_INCR_WRAP;

        case PIPE_STENCIL_OP_DECR_WRAP:
                return MALI_STENCIL_DECR_WRAP;

        case PIPE_STENCIL_OP_INVERT:
                return MALI_STENCIL_INVERT;

        default:
                unreachable("Invalid stencil op");
        }
}

static void
panfrost_make_stencil_state(const struct pipe_stencil_state *in, struct mali_stencil_test *out)
{
        out->ref = 0; /* Gallium gets it from elsewhere */

        out->mask = in->valuemask;
        out->func = panfrost_translate_compare_func(in->func);
        out->sfail = panfrost_translate_stencil_op(in->fail_op);
        out->dpfail = panfrost_translate_stencil_op(in->zfail_op);
        out->dppass = panfrost_translate_stencil_op(in->zpass_op);
}

static void
panfrost_default_shader_backend(struct panfrost_context *ctx)
{
        struct mali_shader_meta shader = {
                .alpha_coverage = ~MALI_ALPHA_COVERAGE(0.000000),

                .unknown2_3 = MALI_DEPTH_FUNC(MALI_FUNC_ALWAYS) | 0x3010,
                .unknown2_4 = MALI_NO_MSAA | 0x4e0,
        };

        /* unknown2_4 has 0x10 bit set on T6XX. We don't know why this is
         * required (independent of 32-bit/64-bit descriptors), or why it's not
         * used on later GPU revisions. Otherwise, all shader jobs fault on
         * these earlier chips (perhaps this is a chicken bit of some kind).
         * More investigation is needed. */

	if (ctx->is_t6xx) {
		shader.unknown2_4 |= 0x10;
	}

        struct pipe_stencil_state default_stencil = {
                .enabled = 0,
                .func = PIPE_FUNC_ALWAYS,
                .fail_op = MALI_STENCIL_KEEP,
                .zfail_op = MALI_STENCIL_KEEP,
                .zpass_op = MALI_STENCIL_KEEP,
                .writemask = 0xFF,
                .valuemask = 0xFF
        };

        panfrost_make_stencil_state(&default_stencil, &shader.stencil_front);
        shader.stencil_mask_front = default_stencil.writemask;

        panfrost_make_stencil_state(&default_stencil, &shader.stencil_back);
        shader.stencil_mask_back = default_stencil.writemask;

        if (default_stencil.enabled)
                shader.unknown2_4 |= MALI_STENCIL_TEST;

        memcpy(&ctx->fragment_shader_core, &shader, sizeof(shader));
}

/* Generates a vertex/tiler job. This is, in some sense, the heart of the
 * graphics command stream. It should be called once per draw, accordding to
 * presentations. Set is_tiler for "tiler" jobs (fragment shader jobs, but in
 * Mali parlance, "fragment" refers to framebuffer writeout). Clear it for
 * vertex jobs. */

struct panfrost_transfer
panfrost_vertex_tiler_job(struct panfrost_context *ctx, bool is_tiler)
{
        struct mali_job_descriptor_header job = {
                .job_type = is_tiler ? JOB_TYPE_TILER : JOB_TYPE_VERTEX,
                .job_descriptor_size = 1,
        };

        struct midgard_payload_vertex_tiler *payload = is_tiler ? &ctx->payloads[PIPE_SHADER_FRAGMENT] : &ctx->payloads[PIPE_SHADER_VERTEX];

        struct panfrost_transfer transfer = panfrost_allocate_transient(ctx, sizeof(job) + sizeof(*payload));
        memcpy(transfer.cpu, &job, sizeof(job));
        memcpy(transfer.cpu + sizeof(job), payload, sizeof(*payload));
        return transfer;
}

static mali_ptr
panfrost_emit_varyings(
        struct panfrost_context *ctx,
        union mali_attr *slot,
        unsigned stride,
        unsigned count)
{
        /* Fill out the descriptor */
        slot->stride = stride;
        slot->size = stride * count;
        slot->shift = slot->extra_flags = 0;

        struct panfrost_transfer transfer =
                panfrost_allocate_transient(ctx, slot->size);

        slot->elements = transfer.gpu | MALI_ATTR_LINEAR;

        return transfer.gpu;
}

static void
panfrost_emit_point_coord(union mali_attr *slot)
{
        slot->elements = MALI_VARYING_POINT_COORD | MALI_ATTR_LINEAR;
        slot->stride = slot->size = slot->shift = slot->extra_flags = 0;
}

static void
panfrost_emit_front_face(union mali_attr *slot)
{
        slot->elements = MALI_VARYING_FRONT_FACING | MALI_ATTR_INTERNAL;
}

static void
panfrost_emit_varying_descriptor(
        struct panfrost_context *ctx,
        unsigned vertex_count)
{
        /* Load the shaders */

        struct panfrost_shader_state *vs = &ctx->shader[PIPE_SHADER_VERTEX]->variants[ctx->shader[PIPE_SHADER_VERTEX]->active_variant];
        struct panfrost_shader_state *fs = &ctx->shader[PIPE_SHADER_FRAGMENT]->variants[ctx->shader[PIPE_SHADER_FRAGMENT]->active_variant];
        unsigned int num_gen_varyings = 0;

        /* Allocate the varying descriptor */

        size_t vs_size = sizeof(struct mali_attr_meta) * vs->tripipe->varying_count;
        size_t fs_size = sizeof(struct mali_attr_meta) * fs->tripipe->varying_count;

        struct panfrost_transfer trans = panfrost_allocate_transient(ctx,
                                         vs_size + fs_size);

        /*
         * Assign ->src_offset now that we know about all the general purpose
         * varyings that will be used by the fragment and vertex shaders.
         */
        for (unsigned i = 0; i < vs->tripipe->varying_count; i++) {
                /*
                 * General purpose varyings have ->index set to 0, skip other
                 * entries.
                 */
                if (vs->varyings[i].index)
                        continue;

                vs->varyings[i].src_offset = 16 * (num_gen_varyings++);
        }

        for (unsigned i = 0; i < fs->tripipe->varying_count; i++) {
                unsigned j;

                /* If we have a point sprite replacement, handle that here. We
                 * have to translate location first.  TODO: Flip y in shader.
                 * We're already keying ... just time crunch .. */

                unsigned loc = fs->varyings_loc[i];
                unsigned pnt_loc =
                        (loc >= VARYING_SLOT_VAR0) ? (loc - VARYING_SLOT_VAR0) :
                        (loc == VARYING_SLOT_PNTC) ? 8 :
                        ~0;

                if (~pnt_loc && fs->point_sprite_mask & (1 << pnt_loc)) {
                        /* gl_PointCoord index by convention */
                        fs->varyings[i].index = 3;
                        fs->reads_point_coord = true;

                        /* Swizzle out the z/w to 0/1 */
                        fs->varyings[i].format = MALI_RG16F;
                        fs->varyings[i].swizzle =
                                panfrost_get_default_swizzle(2);

                        continue;
                }

                if (fs->varyings[i].index)
                        continue;

                /*
                 * Re-use the VS general purpose varying pos if it exists,
                 * create a new one otherwise.
                 */
                for (j = 0; j < vs->tripipe->varying_count; j++) {
                        if (fs->varyings_loc[i] == vs->varyings_loc[j])
                                break;
                }

                if (j < vs->tripipe->varying_count)
                        fs->varyings[i].src_offset = vs->varyings[j].src_offset;
                else
                        fs->varyings[i].src_offset = 16 * (num_gen_varyings++);
        }

        memcpy(trans.cpu, vs->varyings, vs_size);
        memcpy(trans.cpu + vs_size, fs->varyings, fs_size);

        ctx->payloads[PIPE_SHADER_VERTEX].postfix.varying_meta = trans.gpu;
        ctx->payloads[PIPE_SHADER_FRAGMENT].postfix.varying_meta = trans.gpu + vs_size;

        /* Buffer indices must be in this order per our convention */
        union mali_attr varyings[PIPE_MAX_ATTRIBS];
        unsigned idx = 0;

        panfrost_emit_varyings(ctx, &varyings[idx++], num_gen_varyings * 16,
                               vertex_count);

        /* fp32 vec4 gl_Position */
        ctx->payloads[PIPE_SHADER_FRAGMENT].postfix.position_varying =
                panfrost_emit_varyings(ctx, &varyings[idx++],
                                       sizeof(float) * 4, vertex_count);


        if (vs->writes_point_size || fs->reads_point_coord) {
                /* fp16 vec1 gl_PointSize */
                ctx->payloads[PIPE_SHADER_FRAGMENT].primitive_size.pointer =
                        panfrost_emit_varyings(ctx, &varyings[idx++],
                                               2, vertex_count);
        } else if (fs->reads_face) {
                /* Dummy to advance index */
                ++idx;
        }

        if (fs->reads_point_coord) {
                /* Special descriptor */
                panfrost_emit_point_coord(&varyings[idx++]);
        } else if (fs->reads_face) {
                ++idx;
        }

        if (fs->reads_face) {
                panfrost_emit_front_face(&varyings[idx++]);
        }

        mali_ptr varyings_p = panfrost_upload_transient(ctx, &varyings, idx * sizeof(union mali_attr));
        ctx->payloads[PIPE_SHADER_VERTEX].postfix.varyings = varyings_p;
        ctx->payloads[PIPE_SHADER_FRAGMENT].postfix.varyings = varyings_p;
}

mali_ptr
panfrost_vertex_buffer_address(struct panfrost_context *ctx, unsigned i)
{
        struct pipe_vertex_buffer *buf = &ctx->vertex_buffers[i];
        struct panfrost_resource *rsrc = (struct panfrost_resource *) (buf->buffer.resource);

        return rsrc->bo->gpu + buf->buffer_offset;
}

static bool
panfrost_writes_point_size(struct panfrost_context *ctx)
{
        assert(ctx->shader[PIPE_SHADER_VERTEX]);
        struct panfrost_shader_state *vs = &ctx->shader[PIPE_SHADER_VERTEX]->variants[ctx->shader[PIPE_SHADER_VERTEX]->active_variant];

        return vs->writes_point_size && ctx->payloads[PIPE_SHADER_FRAGMENT].prefix.draw_mode == MALI_POINTS;
}

/* Stage the attribute descriptors so we can adjust src_offset
 * to let BOs align nicely */

static void
panfrost_stage_attributes(struct panfrost_context *ctx)
{
        struct panfrost_vertex_state *so = ctx->vertex;

        size_t sz = sizeof(struct mali_attr_meta) * so->num_elements;
        struct panfrost_transfer transfer = panfrost_allocate_transient(ctx, sz);
        struct mali_attr_meta *target = (struct mali_attr_meta *) transfer.cpu;

        /* Copy as-is for the first pass */
        memcpy(target, so->hw, sz);

        /* Fixup offsets for the second pass. Recall that the hardware
         * calculates attribute addresses as:
         *
         *      addr = base + (stride * vtx) + src_offset;
         *
         * However, on Mali, base must be aligned to 64-bytes, so we
         * instead let:
         *
         *      base' = base & ~63 = base - (base & 63)
         *
         * To compensate when using base' (see emit_vertex_data), we have
         * to adjust src_offset by the masked off piece:
         *
         *      addr' = base' + (stride * vtx) + (src_offset + (base & 63))
         *            = base - (base & 63) + (stride * vtx) + src_offset + (base & 63)
         *            = base + (stride * vtx) + src_offset
         *            = addr;
         *
         * QED.
         */

        unsigned start = ctx->payloads[PIPE_SHADER_VERTEX].draw_start;

        for (unsigned i = 0; i < so->num_elements; ++i) {
                unsigned vbi = so->pipe[i].vertex_buffer_index;
                struct pipe_vertex_buffer *buf = &ctx->vertex_buffers[vbi];
                mali_ptr addr = panfrost_vertex_buffer_address(ctx, vbi);

                /* Adjust by the masked off bits of the offset */
                target[i].src_offset += (addr & 63);

                /* Also, somewhat obscurely per-instance data needs to be
                 * offset in response to a delayed start in an indexed draw */

                if (so->pipe[i].instance_divisor && ctx->instance_count > 1 && start) {
                        target[i].src_offset -= buf->stride * start;
                }


        }

        ctx->payloads[PIPE_SHADER_VERTEX].postfix.attribute_meta = transfer.gpu;
}

static void
panfrost_upload_sampler_descriptors(struct panfrost_context *ctx)
{
        size_t desc_size = sizeof(struct mali_sampler_descriptor);

        for (int t = 0; t <= PIPE_SHADER_FRAGMENT; ++t) {
                mali_ptr upload = 0;

                if (ctx->sampler_count[t] && ctx->sampler_view_count[t]) {
                        size_t transfer_size = desc_size * ctx->sampler_count[t];

                        struct panfrost_transfer transfer =
                                panfrost_allocate_transient(ctx, transfer_size);

                        struct mali_sampler_descriptor *desc =
                                (struct mali_sampler_descriptor *) transfer.cpu;

                        for (int i = 0; i < ctx->sampler_count[t]; ++i)
                                desc[i] = ctx->samplers[t][i]->hw;

                        upload = transfer.gpu;
                }

                ctx->payloads[t].postfix.sampler_descriptor = upload;
        }
}

static unsigned
panfrost_layout_for_texture(struct panfrost_resource *rsrc, bool manual_stride)
{
        /* TODO: other linear depth textures */
        bool is_depth = rsrc->base.format == PIPE_FORMAT_Z32_UNORM;

        unsigned usage2_layout = 0x10;

        switch (rsrc->layout) {
        case PAN_AFBC:
                usage2_layout |= 0x8 | 0x4;
                break;
        case PAN_TILED:
                usage2_layout |= 0x1;
                break;
        case PAN_LINEAR:
                usage2_layout |= is_depth ? 0x1 : 0x2;
                break;
        default:
                assert(0);
                break;
        }

        if (manual_stride)
                usage2_layout |= MALI_TEX_MANUAL_STRIDE;

        return usage2_layout;
}

static mali_ptr
panfrost_upload_tex(
        struct panfrost_context *ctx,
        struct panfrost_sampler_view *view)
{
        if (!view)
                return (mali_ptr) 0;

        struct pipe_sampler_view *pview = &view->base;
        struct panfrost_resource *rsrc = pan_resource(pview->texture);

        /* Do we interleave an explicit stride with every element? */

        bool has_manual_stride = view->manual_stride;

        /* For easy access */

        assert(pview->target != PIPE_BUFFER);
        unsigned first_level = pview->u.tex.first_level;
        unsigned last_level = pview->u.tex.last_level;
        unsigned first_layer = pview->u.tex.first_layer;
        unsigned last_layer = pview->u.tex.last_layer;

        /* Lower-bit is set when sampling from colour AFBC */
        bool is_afbc = rsrc->layout == PAN_AFBC;
        bool is_zs = rsrc->base.bind & PIPE_BIND_DEPTH_STENCIL;
        unsigned afbc_bit = (is_afbc && !is_zs) ? 1 : 0;

        /* Add the BO to the job so it's retained until the job is done. */
        struct panfrost_job *job = panfrost_get_job_for_fbo(ctx);
        panfrost_job_add_bo(job, rsrc->bo);

        /* Add the usage flags in, since they can change across the CSO
         * lifetime due to layout switches */

        view->hw.format.usage2 = panfrost_layout_for_texture(rsrc, has_manual_stride);

        /* Inject the addresses in, interleaving mip levels, cube faces, and
         * strides in that order */

        unsigned idx = 0;

        for (unsigned l = first_level; l <= last_level; ++l) {
                for (unsigned f = first_layer; f <= last_layer; ++f) {

                        view->hw.payload[idx++] =
                                panfrost_get_texture_address(rsrc, l, f) + afbc_bit;

                        if (has_manual_stride) {
                                view->hw.payload[idx++] =
                                        rsrc->slices[l].stride;
                        }
                }
        }

        return panfrost_upload_transient(ctx, &view->hw,
                                         sizeof(struct mali_texture_descriptor));
}

static void
panfrost_upload_texture_descriptors(struct panfrost_context *ctx)
{
        for (int t = 0; t <= PIPE_SHADER_FRAGMENT; ++t) {
                mali_ptr trampoline = 0;

                if (ctx->sampler_view_count[t]) {
                        uint64_t trampolines[PIPE_MAX_SHADER_SAMPLER_VIEWS];

                        for (int i = 0; i < ctx->sampler_view_count[t]; ++i)
                                trampolines[i] =
                                        panfrost_upload_tex(ctx, ctx->sampler_views[t][i]);

                        trampoline = panfrost_upload_transient(ctx, trampolines, sizeof(uint64_t) * ctx->sampler_view_count[t]);
                }

                ctx->payloads[t].postfix.texture_trampoline = trampoline;
        }
}

struct sysval_uniform {
        union {
                float f[4];
                int32_t i[4];
                uint32_t u[4];
        };
};

static void panfrost_upload_viewport_scale_sysval(struct panfrost_context *ctx,
                struct sysval_uniform *uniform)
{
        const struct pipe_viewport_state *vp = &ctx->pipe_viewport;

        uniform->f[0] = vp->scale[0];
        uniform->f[1] = vp->scale[1];
        uniform->f[2] = vp->scale[2];
}

static void panfrost_upload_viewport_offset_sysval(struct panfrost_context *ctx,
                struct sysval_uniform *uniform)
{
        const struct pipe_viewport_state *vp = &ctx->pipe_viewport;

        uniform->f[0] = vp->translate[0];
        uniform->f[1] = vp->translate[1];
        uniform->f[2] = vp->translate[2];
}

static void panfrost_upload_txs_sysval(struct panfrost_context *ctx,
                                       enum pipe_shader_type st,
                                       unsigned int sysvalid,
                                       struct sysval_uniform *uniform)
{
        unsigned texidx = PAN_SYSVAL_ID_TO_TXS_TEX_IDX(sysvalid);
        unsigned dim = PAN_SYSVAL_ID_TO_TXS_DIM(sysvalid);
        bool is_array = PAN_SYSVAL_ID_TO_TXS_IS_ARRAY(sysvalid);
        struct pipe_sampler_view *tex = &ctx->sampler_views[st][texidx]->base;

        assert(dim);
        uniform->i[0] = u_minify(tex->texture->width0, tex->u.tex.first_level);

        if (dim > 1)
                uniform->i[1] = u_minify(tex->texture->height0,
                                         tex->u.tex.first_level);

        if (dim > 2)
                uniform->i[2] = u_minify(tex->texture->depth0,
                                         tex->u.tex.first_level);

        if (is_array)
                uniform->i[dim] = tex->texture->array_size;
}

static void panfrost_upload_sysvals(struct panfrost_context *ctx, void *buf,
                                    struct panfrost_shader_state *ss,
                                    enum pipe_shader_type st)
{
        struct sysval_uniform *uniforms = (void *)buf;

        for (unsigned i = 0; i < ss->sysval_count; ++i) {
                int sysval = ss->sysval[i];

                switch (PAN_SYSVAL_TYPE(sysval)) {
                case PAN_SYSVAL_VIEWPORT_SCALE:
                        panfrost_upload_viewport_scale_sysval(ctx, &uniforms[i]);
                        break;
                case PAN_SYSVAL_VIEWPORT_OFFSET:
                        panfrost_upload_viewport_offset_sysval(ctx, &uniforms[i]);
                        break;
                case PAN_SYSVAL_TEXTURE_SIZE:
                        panfrost_upload_txs_sysval(ctx, st, PAN_SYSVAL_ID(sysval),
                                                   &uniforms[i]);
                        break;
                default:
                        assert(0);
                }
        }
}

static const void *
panfrost_map_constant_buffer_cpu(struct panfrost_constant_buffer *buf, unsigned index)
{
        struct pipe_constant_buffer *cb = &buf->cb[index];
        struct panfrost_resource *rsrc = pan_resource(cb->buffer);

        if (rsrc)
                return rsrc->bo->cpu;
        else if (cb->user_buffer)
                return cb->user_buffer;
        else
                unreachable("No constant buffer");
}

static mali_ptr
panfrost_map_constant_buffer_gpu(
        struct panfrost_context *ctx,
        struct panfrost_constant_buffer *buf,
        unsigned index)
{
        struct pipe_constant_buffer *cb = &buf->cb[index];
        struct panfrost_resource *rsrc = pan_resource(cb->buffer);

        if (rsrc)
                return rsrc->bo->gpu;
        else if (cb->user_buffer)
                return panfrost_upload_transient(ctx, cb->user_buffer, cb->buffer_size);
        else
                unreachable("No constant buffer");
}

/* Compute number of UBOs active (more specifically, compute the highest UBO
 * number addressable -- if there are gaps, include them in the count anyway).
 * We always include UBO #0 in the count, since we *need* uniforms enabled for
 * sysvals. */

static unsigned
panfrost_ubo_count(struct panfrost_context *ctx, enum pipe_shader_type stage)
{
        unsigned mask = ctx->constant_buffer[stage].enabled_mask | 1;
        return 32 - __builtin_clz(mask);
}

/* Fixes up a shader state with current state, returning a GPU address to the
 * patched shader */

static mali_ptr
panfrost_patch_shader_state(
        struct panfrost_context *ctx,
        struct panfrost_shader_state *ss,
        enum pipe_shader_type stage,
        bool should_upload)
{
        ss->tripipe->texture_count = ctx->sampler_view_count[stage];
        ss->tripipe->sampler_count = ctx->sampler_count[stage];

        ss->tripipe->midgard1.flags = 0x220;

        unsigned ubo_count = panfrost_ubo_count(ctx, stage);
        ss->tripipe->midgard1.uniform_buffer_count = ubo_count;

        /* We can't reuse over frames; that's not safe. The descriptor must be
         * transient uploaded */

        if (should_upload) {
                return panfrost_upload_transient(ctx,
                                ss->tripipe,
                                sizeof(struct mali_shader_meta));
        }

        /* If we don't need an upload, don't bother */
        return 0;

}

static void
panfrost_patch_shader_state_compute(
        struct panfrost_context *ctx,
        enum pipe_shader_type stage,
        bool should_upload)
{
        struct panfrost_shader_variants *all = ctx->shader[stage];

        if (!all) {
                ctx->payloads[stage].postfix._shader_upper = 0;
                return;
        }

        struct panfrost_shader_state *s = &all->variants[all->active_variant];

        ctx->payloads[stage].postfix._shader_upper =
                panfrost_patch_shader_state(ctx, s, stage, should_upload) >> 4;
}

/* Go through dirty flags and actualise them in the cmdstream. */

void
panfrost_emit_for_draw(struct panfrost_context *ctx, bool with_vertex_data)
{
        struct panfrost_job *job = panfrost_get_job_for_fbo(ctx);
        struct panfrost_screen *screen = pan_screen(ctx->base.screen);

        panfrost_attach_vt_framebuffer(ctx);

        if (with_vertex_data) {
                panfrost_emit_vertex_data(job);

                /* Varyings emitted for -all- geometry */
                unsigned total_count = ctx->padded_count * ctx->instance_count;
                panfrost_emit_varying_descriptor(ctx, total_count);
        }

        bool msaa = ctx->rasterizer->base.multisample;

        if (ctx->dirty & PAN_DIRTY_RASTERIZER) {
                ctx->payloads[PIPE_SHADER_FRAGMENT].gl_enables = ctx->rasterizer->tiler_gl_enables;

                /* TODO: Sample size */
                SET_BIT(ctx->fragment_shader_core.unknown2_3, MALI_HAS_MSAA, msaa);
                SET_BIT(ctx->fragment_shader_core.unknown2_4, MALI_NO_MSAA, !msaa);
        }

        panfrost_job_set_requirements(ctx, job);

        if (ctx->occlusion_query) {
                ctx->payloads[PIPE_SHADER_FRAGMENT].gl_enables |= MALI_OCCLUSION_QUERY | MALI_OCCLUSION_PRECISE;
                ctx->payloads[PIPE_SHADER_FRAGMENT].postfix.occlusion_counter = ctx->occlusion_query->transfer.gpu;
        }

        panfrost_patch_shader_state_compute(ctx, PIPE_SHADER_VERTEX, true);
        panfrost_patch_shader_state_compute(ctx, PIPE_SHADER_COMPUTE, true);

        if (ctx->dirty & (PAN_DIRTY_RASTERIZER | PAN_DIRTY_VS)) {
                /* Check if we need to link the gl_PointSize varying */
                if (!panfrost_writes_point_size(ctx)) {
                        /* If the size is constant, write it out. Otherwise,
                         * don't touch primitive_size (since we would clobber
                         * the pointer there) */

                        ctx->payloads[PIPE_SHADER_FRAGMENT].primitive_size.constant = ctx->rasterizer->base.line_width;
                }
        }

        /* TODO: Maybe dirty track FS, maybe not. For now, it's transient. */
        if (ctx->shader[PIPE_SHADER_FRAGMENT])
                ctx->dirty |= PAN_DIRTY_FS;

        if (ctx->dirty & PAN_DIRTY_FS) {
                assert(ctx->shader[PIPE_SHADER_FRAGMENT]);
                struct panfrost_shader_state *variant = &ctx->shader[PIPE_SHADER_FRAGMENT]->variants[ctx->shader[PIPE_SHADER_FRAGMENT]->active_variant];

                panfrost_patch_shader_state(ctx, variant, PIPE_SHADER_FRAGMENT, false);

#define COPY(name) ctx->fragment_shader_core.name = variant->tripipe->name

                COPY(shader);
                COPY(attribute_count);
                COPY(varying_count);
                COPY(texture_count);
                COPY(sampler_count);
                COPY(sampler_count);
                COPY(midgard1.uniform_count);
                COPY(midgard1.uniform_buffer_count);
                COPY(midgard1.work_count);
                COPY(midgard1.flags);
                COPY(midgard1.unknown2);

#undef COPY

                /* Get blending setup */
                struct panfrost_blend_final blend =
                        panfrost_get_blend_for_context(ctx, 0);

                /* If there is a blend shader, work registers are shared */

                if (blend.is_shader)
                        ctx->fragment_shader_core.midgard1.work_count = /*MAX2(ctx->fragment_shader_core.midgard1.work_count, ctx->blend->blend_work_count)*/16;

                /* Set late due to depending on render state */
                unsigned flags = ctx->fragment_shader_core.midgard1.flags;

                /* Depending on whether it's legal to in the given shader, we
                 * try to enable early-z testing (or forward-pixel kill?) */

                if (!variant->can_discard)
                        flags |= MALI_EARLY_Z;

                /* Any time texturing is used, derivatives are implicitly
                 * calculated, so we need to enable helper invocations */

                if (variant->helper_invocations)
                        flags |= MALI_HELPER_INVOCATIONS;

                ctx->fragment_shader_core.midgard1.flags = flags;

                /* Assign the stencil refs late */

                unsigned front_ref = ctx->stencil_ref.ref_value[0];
                unsigned back_ref = ctx->stencil_ref.ref_value[1];
                bool back_enab = ctx->depth_stencil->stencil[1].enabled;

                ctx->fragment_shader_core.stencil_front.ref = front_ref;
                ctx->fragment_shader_core.stencil_back.ref = back_enab ? back_ref : front_ref;

                /* CAN_DISCARD should be set if the fragment shader possibly
                 * contains a 'discard' instruction. It is likely this is
                 * related to optimizations related to forward-pixel kill, as
                 * per "Mali Performance 3: Is EGL_BUFFER_PRESERVED a good
                 * thing?" by Peter Harris
                 */

                if (variant->can_discard) {
                        ctx->fragment_shader_core.unknown2_3 |= MALI_CAN_DISCARD;
                        ctx->fragment_shader_core.midgard1.flags |= 0x400;
                }

                /* Check if we're using the default blend descriptor (fast path) */

                bool no_blending =
                        !blend.is_shader &&
                        (blend.equation.equation->rgb_mode == 0x122) &&
                        (blend.equation.equation->alpha_mode == 0x122) &&
                        (blend.equation.equation->color_mask == 0xf);

                /* Even on MFBD, the shader descriptor gets blend shaders. It's
                 * *also* copied to the blend_meta appended (by convention),
                 * but this is the field actually read by the hardware. (Or
                 * maybe both are read...?) */

                if (blend.is_shader) {
                        ctx->fragment_shader_core.blend.shader =
                                blend.shader.gpu;
                } else {
                        ctx->fragment_shader_core.blend.shader = 0;
                }

                if (screen->require_sfbd) {
                        /* When only a single render target platform is used, the blend
                         * information is inside the shader meta itself. We
                         * additionally need to signal CAN_DISCARD for nontrivial blend
                         * modes (so we're able to read back the destination buffer) */

                        if (!blend.is_shader) {
                                ctx->fragment_shader_core.blend.equation =
                                        *blend.equation.equation;
                                ctx->fragment_shader_core.blend.constant =
                                        blend.equation.constant;
                        }

                        if (!no_blending) {
                                ctx->fragment_shader_core.unknown2_3 |= MALI_CAN_DISCARD;
                        }
                }

                size_t size = sizeof(struct mali_shader_meta) + sizeof(struct midgard_blend_rt);
                struct panfrost_transfer transfer = panfrost_allocate_transient(ctx, size);
                memcpy(transfer.cpu, &ctx->fragment_shader_core, sizeof(struct mali_shader_meta));

                ctx->payloads[PIPE_SHADER_FRAGMENT].postfix._shader_upper = (transfer.gpu) >> 4;

                if (!screen->require_sfbd) {
                        /* Additional blend descriptor tacked on for jobs using MFBD */

                        unsigned blend_count = 0x200;

                        if (blend.is_shader) {
                                /* For a blend shader, the bottom nibble corresponds to
                                 * the number of work registers used, which signals the
                                 * -existence- of a blend shader */

                                assert(blend.shader.work_count >= 2);
                                blend_count |= MIN2(blend.shader.work_count, 3);
                        } else {
                                /* Otherwise, the bottom bit simply specifies if
                                 * blending (anything other than REPLACE) is enabled */


                                if (!no_blending)
                                        blend_count |= 0x1;
                        }

                        struct midgard_blend_rt rts[4];

                        for (unsigned i = 0; i < ctx->pipe_framebuffer.nr_cbufs; ++i) {
                                bool is_srgb =
                                        (ctx->pipe_framebuffer.nr_cbufs > i) &&
                                        (ctx->pipe_framebuffer.cbufs[i]) &&
                                        util_format_is_srgb(ctx->pipe_framebuffer.cbufs[i]->format);

                                rts[i].flags = blend_count;

                                if (is_srgb)
                                        rts[i].flags |= MALI_BLEND_SRGB;

                                if (!ctx->blend->base.dither)
                                        rts[i].flags |= MALI_BLEND_NO_DITHER;

                                /* TODO: sRGB in blend shaders is currently
                                 * unimplemented. Contact me (Alyssa) if you're
                                 * interested in working on this. We have
                                 * native Midgard ops for helping here, but
                                 * they're not well-understood yet. */

                                assert(!(is_srgb && blend.is_shader));

                                if (blend.is_shader) {
                                        rts[i].blend.shader = blend.shader.gpu;
                                } else {
                                        rts[i].blend.equation = *blend.equation.equation;
                                        rts[i].blend.constant = blend.equation.constant;
                                }
                        }

                        memcpy(transfer.cpu + sizeof(struct mali_shader_meta), rts, sizeof(rts[0]) * 1);
                }
        }

        /* We stage to transient, so always dirty.. */
        if (ctx->vertex)
                panfrost_stage_attributes(ctx);

        if (ctx->dirty & PAN_DIRTY_SAMPLERS)
                panfrost_upload_sampler_descriptors(ctx);

        if (ctx->dirty & PAN_DIRTY_TEXTURES)
                panfrost_upload_texture_descriptors(ctx);

        const struct pipe_viewport_state *vp = &ctx->pipe_viewport;

        for (int i = 0; i < PIPE_SHADER_TYPES; ++i) {
                struct panfrost_shader_variants *all = ctx->shader[i];

                if (!all)
                        continue;

                struct panfrost_constant_buffer *buf = &ctx->constant_buffer[i];

                struct panfrost_shader_state *ss = &all->variants[all->active_variant];

                /* Uniforms are implicitly UBO #0 */
                bool has_uniforms = buf->enabled_mask & (1 << 0);

                /* Allocate room for the sysval and the uniforms */
                size_t sys_size = sizeof(float) * 4 * ss->sysval_count;
                size_t uniform_size = has_uniforms ? (buf->cb[0].buffer_size) : 0;
                size_t size = sys_size + uniform_size;
                struct panfrost_transfer transfer = panfrost_allocate_transient(ctx, size);

                /* Upload sysvals requested by the shader */
                panfrost_upload_sysvals(ctx, transfer.cpu, ss, i);

                /* Upload uniforms */
                if (has_uniforms) {
                        const void *cpu = panfrost_map_constant_buffer_cpu(buf, 0);
                        memcpy(transfer.cpu + sys_size, cpu, uniform_size);
                }

                int uniform_count =
                        ctx->shader[i]->variants[ctx->shader[i]->active_variant].uniform_count;

                struct mali_vertex_tiler_postfix *postfix =
                        &ctx->payloads[i].postfix;

                /* Next up, attach UBOs. UBO #0 is the uniforms we just
                 * uploaded */

                unsigned ubo_count = panfrost_ubo_count(ctx, i);
                assert(ubo_count >= 1);

                size_t sz = sizeof(struct mali_uniform_buffer_meta) * ubo_count;
                struct mali_uniform_buffer_meta ubos[PAN_MAX_CONST_BUFFERS];

                /* Upload uniforms as a UBO */
                ubos[0].size = MALI_POSITIVE((2 + uniform_count));
                ubos[0].ptr = transfer.gpu >> 2;

                /* The rest are honest-to-goodness UBOs */

                for (unsigned ubo = 1; ubo < ubo_count; ++ubo) {
                        size_t sz = buf->cb[ubo].buffer_size;

                        bool enabled = buf->enabled_mask & (1 << ubo);
                        bool empty = sz == 0;

                        if (!enabled || empty) {
                                /* Stub out disabled UBOs to catch accesses */

                                ubos[ubo].size = 0;
                                ubos[ubo].ptr = 0xDEAD0000;
                                continue;
                        }

                        mali_ptr gpu = panfrost_map_constant_buffer_gpu(ctx, buf, ubo);

                        unsigned bytes_per_field = 16;
                        unsigned aligned = ALIGN_POT(sz, bytes_per_field);
                        unsigned fields = aligned / bytes_per_field;

                        ubos[ubo].size = MALI_POSITIVE(fields);
                        ubos[ubo].ptr = gpu >> 2;
                }

                mali_ptr ubufs = panfrost_upload_transient(ctx, ubos, sz);
                postfix->uniforms = transfer.gpu;
                postfix->uniform_buffers = ubufs;

                buf->dirty_mask = 0;
        }

        /* TODO: Upload the viewport somewhere more appropriate */

        /* Clip bounds are encoded as floats. The viewport itself is encoded as
         * (somewhat) asymmetric ints. */
        const struct pipe_scissor_state *ss = &ctx->scissor;

        struct mali_viewport view = {
                /* By default, do no viewport clipping, i.e. clip to (-inf,
                 * inf) in each direction. Clipping to the viewport in theory
                 * should work, but in practice causes issues when we're not
                 * explicitly trying to scissor */

                .clip_minx = -INFINITY,
                .clip_miny = -INFINITY,
                .clip_maxx = INFINITY,
                .clip_maxy = INFINITY,

                .clip_minz = 0.0,
                .clip_maxz = 1.0,
        };

        /* Always scissor to the viewport by default. */
        float vp_minx = (int) (vp->translate[0] - fabsf(vp->scale[0]));
        float vp_maxx = (int) (vp->translate[0] + fabsf(vp->scale[0]));

        float vp_miny = (int) (vp->translate[1] - fabsf(vp->scale[1]));
        float vp_maxy = (int) (vp->translate[1] + fabsf(vp->scale[1]));

        /* Apply the scissor test */

        unsigned minx, miny, maxx, maxy;

        if (ss && ctx->rasterizer && ctx->rasterizer->base.scissor) {
                minx = MAX2(ss->minx, vp_minx);
                miny = MAX2(ss->miny, vp_miny);
                maxx = MIN2(ss->maxx, vp_maxx);
                maxy = MIN2(ss->maxy, vp_maxy);
        } else {
                minx = vp_minx;
                miny = vp_miny;
                maxx = vp_maxx;
                maxy = vp_maxy;
        }

        /* Hardware needs the min/max to be strictly ordered, so flip if we
         * need to. The viewport transformation in the vertex shader will
         * handle the negatives if we don't */

        if (miny > maxy) {
                int temp = miny;
                miny = maxy;
                maxy = temp;
        }

        if (minx > maxx) {
                int temp = minx;
                minx = maxx;
                maxx = temp;
        }

        /* Clamp everything positive, just in case */

        maxx = MAX2(0, maxx);
        maxy = MAX2(0, maxy);
        minx = MAX2(0, minx);
        miny = MAX2(0, miny);

        /* Clamp to the framebuffer size as a last check */

        minx = MIN2(ctx->pipe_framebuffer.width, minx);
        maxx = MIN2(ctx->pipe_framebuffer.width, maxx);

        miny = MIN2(ctx->pipe_framebuffer.height, miny);
        maxy = MIN2(ctx->pipe_framebuffer.height, maxy);

        /* Update the job, unless we're doing wallpapering (whose lack of
         * scissor we can ignore, since if we "miss" a tile of wallpaper, it'll
         * just... be faster :) */

        if (!ctx->wallpaper_batch)
                panfrost_job_union_scissor(job, minx, miny, maxx, maxy);

        /* Upload */

        view.viewport0[0] = minx;
        view.viewport1[0] = MALI_POSITIVE(maxx);

        view.viewport0[1] = miny;
        view.viewport1[1] = MALI_POSITIVE(maxy);

        ctx->payloads[PIPE_SHADER_FRAGMENT].postfix.viewport =
                panfrost_upload_transient(ctx,
                                          &view,
                                          sizeof(struct mali_viewport));

        ctx->dirty = 0;
}

/* Corresponds to exactly one draw, but does not submit anything */

static void
panfrost_queue_draw(struct panfrost_context *ctx)
{
        /* Handle dirty flags now */
        panfrost_emit_for_draw(ctx, true);

        /* If rasterizer discard is enable, only submit the vertex */

        bool rasterizer_discard = ctx->rasterizer
                                  && ctx->rasterizer->base.rasterizer_discard;

        struct panfrost_transfer vertex = panfrost_vertex_tiler_job(ctx, false);
        struct panfrost_transfer tiler;

        if (!rasterizer_discard)
                tiler = panfrost_vertex_tiler_job(ctx, true);

        struct panfrost_job *batch = panfrost_get_job_for_fbo(ctx);

        if (rasterizer_discard)
                panfrost_scoreboard_queue_vertex_job(batch, vertex, FALSE);
        else if (ctx->wallpaper_batch)
                panfrost_scoreboard_queue_fused_job_prepend(batch, vertex, tiler);
        else
                panfrost_scoreboard_queue_fused_job(batch, vertex, tiler);
}

/* The entire frame is in memory -- send it off to the kernel! */

static void
panfrost_submit_frame(struct panfrost_context *ctx, bool flush_immediate,
                      struct pipe_fence_handle **fence,
                      struct panfrost_job *job)
{
        struct pipe_context *gallium = (struct pipe_context *) ctx;
        struct panfrost_screen *screen = pan_screen(gallium->screen);

#ifndef DRY_RUN

        panfrost_job_submit(ctx, job);

        /* If visual, we can stall a frame */

        if (!flush_immediate)
                panfrost_drm_force_flush_fragment(ctx, fence);

        screen->last_fragment_flushed = false;
        screen->last_job = job;

        /* If readback, flush now (hurts the pipelined performance) */
        if (flush_immediate)
                panfrost_drm_force_flush_fragment(ctx, fence);
#endif
}

static void
panfrost_draw_wallpaper(struct pipe_context *pipe)
{
        struct panfrost_context *ctx = pan_context(pipe);

        /* Nothing to reload? TODO: MRT wallpapers */
        if (ctx->pipe_framebuffer.cbufs[0] == NULL)
                return;

        /* Check if the buffer has any content on it worth preserving */

        struct pipe_surface *surf = ctx->pipe_framebuffer.cbufs[0];
        struct panfrost_resource *rsrc = pan_resource(surf->texture);
        unsigned level = surf->u.tex.level;

        if (!rsrc->slices[level].initialized)
                return;

        /* Save the batch */
        struct panfrost_job *batch = panfrost_get_job_for_fbo(ctx);

        ctx->wallpaper_batch = batch;
        panfrost_blit_wallpaper(ctx);
        ctx->wallpaper_batch = NULL;
}

void
panfrost_flush(
        struct pipe_context *pipe,
        struct pipe_fence_handle **fence,
        unsigned flags)
{
        struct panfrost_context *ctx = pan_context(pipe);
        struct panfrost_job *job = panfrost_get_job_for_fbo(ctx);

        /* Nothing to do! */
        if (!job->last_job.gpu && !job->clear) return;

        if (!job->clear && job->last_tiler.gpu)
                panfrost_draw_wallpaper(&ctx->base);

        /* Whether to stall the pipeline for immediately correct results. Since
         * pipelined rendering is quite broken right now (to be fixed by the
         * panfrost_job refactor, just take the perf hit for correctness) */
        bool flush_immediate = /*flags & PIPE_FLUSH_END_OF_FRAME*/true;

        /* Submit the frame itself */
        panfrost_submit_frame(ctx, flush_immediate, fence, job);

        /* Prepare for the next frame */
        panfrost_invalidate_frame(ctx);
}

#define DEFINE_CASE(c) case PIPE_PRIM_##c: return MALI_##c;

static int
g2m_draw_mode(enum pipe_prim_type mode)
{
        switch (mode) {
                DEFINE_CASE(POINTS);
                DEFINE_CASE(LINES);
                DEFINE_CASE(LINE_LOOP);
                DEFINE_CASE(LINE_STRIP);
                DEFINE_CASE(TRIANGLES);
                DEFINE_CASE(TRIANGLE_STRIP);
                DEFINE_CASE(TRIANGLE_FAN);
                DEFINE_CASE(QUADS);
                DEFINE_CASE(QUAD_STRIP);
                DEFINE_CASE(POLYGON);

        default:
                unreachable("Invalid draw mode");
        }
}

#undef DEFINE_CASE

static unsigned
panfrost_translate_index_size(unsigned size)
{
        switch (size) {
        case 1:
                return MALI_DRAW_INDEXED_UINT8;

        case 2:
                return MALI_DRAW_INDEXED_UINT16;

        case 4:
                return MALI_DRAW_INDEXED_UINT32;

        default:
                unreachable("Invalid index size");
        }
}

/* Gets a GPU address for the associated index buffer. Only gauranteed to be
 * good for the duration of the draw (transient), could last longer */

static mali_ptr
panfrost_get_index_buffer_mapped(struct panfrost_context *ctx, const struct pipe_draw_info *info)
{
        struct panfrost_resource *rsrc = (struct panfrost_resource *) (info->index.resource);

        off_t offset = info->start * info->index_size;
        struct panfrost_job *batch = panfrost_get_job_for_fbo(ctx);

        if (!info->has_user_indices) {
                /* Only resources can be directly mapped */
                panfrost_job_add_bo(batch, rsrc->bo);
                return rsrc->bo->gpu + offset;
        } else {
                /* Otherwise, we need to upload to transient memory */
                const uint8_t *ibuf8 = (const uint8_t *) info->index.user;
                return panfrost_upload_transient(ctx, ibuf8 + offset, info->count * info->index_size);
        }
}

static bool
panfrost_scissor_culls_everything(struct panfrost_context *ctx)
{
        const struct pipe_scissor_state *ss = &ctx->scissor;

        /* Check if we're scissoring at all */

        if (!(ctx->rasterizer && ctx->rasterizer->base.scissor))
                return false;

        return (ss->minx == ss->maxx) || (ss->miny == ss->maxy);
}

static void
panfrost_draw_vbo(
        struct pipe_context *pipe,
        const struct pipe_draw_info *info)
{
        struct panfrost_context *ctx = pan_context(pipe);

        /* First of all, check the scissor to see if anything is drawn at all.
         * If it's not, we drop the draw (mostly a conformance issue;
         * well-behaved apps shouldn't hit this) */

        if (panfrost_scissor_culls_everything(ctx))
                return;

        ctx->payloads[PIPE_SHADER_VERTEX].draw_start = info->start;
        ctx->payloads[PIPE_SHADER_FRAGMENT].draw_start = info->start;

        int mode = info->mode;

        /* Fallback unsupported restart index */
        unsigned primitive_index = (1 << (info->index_size * 8)) - 1;

        if (info->primitive_restart && info->index_size
            && info->restart_index != primitive_index) {
                util_draw_vbo_without_prim_restart(pipe, info);
                return;
        }

        /* Fallback for unsupported modes */

        if (!(ctx->draw_modes & (1 << mode))) {
                if (mode == PIPE_PRIM_QUADS && info->count == 4 && ctx->rasterizer && !ctx->rasterizer->base.flatshade) {
                        mode = PIPE_PRIM_TRIANGLE_FAN;
                } else {
                        if (info->count < 4) {
                                /* Degenerate case? */
                                return;
                        }

                        util_primconvert_save_rasterizer_state(ctx->primconvert, &ctx->rasterizer->base);
                        util_primconvert_draw_vbo(ctx->primconvert, info);
                        return;
                }
        }

        /* Now that we have a guaranteed terminating path, find the job.
         * Assignment commented out to prevent unused warning */

        /* struct panfrost_job *job = */ panfrost_get_job_for_fbo(ctx);

        ctx->payloads[PIPE_SHADER_FRAGMENT].prefix.draw_mode = g2m_draw_mode(mode);

        ctx->vertex_count = info->count;
        ctx->instance_count = info->instance_count;

        /* For non-indexed draws, they're the same */
        unsigned vertex_count = ctx->vertex_count;

        unsigned draw_flags = 0;

        /* The draw flags interpret how primitive size is interpreted */

        if (panfrost_writes_point_size(ctx))
                draw_flags |= MALI_DRAW_VARYING_SIZE;

        if (info->primitive_restart)
                draw_flags |= MALI_DRAW_PRIMITIVE_RESTART_FIXED_INDEX;

        /* For higher amounts of vertices (greater than what fits in a 16-bit
         * short), the other value is needed, otherwise there will be bizarre
         * rendering artefacts. It's not clear what these values mean yet. This
         * change is also needed for instancing and sometimes points (perhaps
         * related to dynamically setting gl_PointSize) */

        bool is_points = mode == PIPE_PRIM_POINTS;
        bool many_verts = ctx->vertex_count > 0xFFFF;
        bool instanced = ctx->instance_count > 1;

        draw_flags |= (is_points || many_verts || instanced) ? 0x3000 : 0x18000;

        /* This doesn't make much sense */
        if (mode == PIPE_PRIM_LINE_STRIP) {
                draw_flags |= 0x800;
        }

        if (info->index_size) {
                /* Calculate the min/max index used so we can figure out how
                 * many times to invoke the vertex shader */

                /* Fetch / calculate index bounds */
                unsigned min_index = 0, max_index = 0;

                if (info->max_index == ~0u) {
                        u_vbuf_get_minmax_index(pipe, info, &min_index, &max_index);
                } else {
                        min_index = info->min_index;
                        max_index = info->max_index;
                }

                /* Use the corresponding values */
                vertex_count = max_index - min_index + 1;
                ctx->payloads[PIPE_SHADER_VERTEX].draw_start = min_index;
                ctx->payloads[PIPE_SHADER_FRAGMENT].draw_start = min_index;

                ctx->payloads[PIPE_SHADER_FRAGMENT].prefix.negative_start = -min_index;
                ctx->payloads[PIPE_SHADER_FRAGMENT].prefix.index_count = MALI_POSITIVE(info->count);

                //assert(!info->restart_index); /* TODO: Research */
                assert(!info->index_bias);

                draw_flags |= panfrost_translate_index_size(info->index_size);
                ctx->payloads[PIPE_SHADER_FRAGMENT].prefix.indices = panfrost_get_index_buffer_mapped(ctx, info);
        } else {
                /* Index count == vertex count, if no indexing is applied, as
                 * if it is internally indexed in the expected order */

                ctx->payloads[PIPE_SHADER_FRAGMENT].prefix.negative_start = 0;
                ctx->payloads[PIPE_SHADER_FRAGMENT].prefix.index_count = MALI_POSITIVE(ctx->vertex_count);

                /* Reverse index state */
                ctx->payloads[PIPE_SHADER_FRAGMENT].prefix.indices = (u64) NULL;
        }

        /* Dispatch "compute jobs" for the vertex/tiler pair as (1,
         * vertex_count, 1) */

        panfrost_pack_work_groups_fused(
                &ctx->payloads[PIPE_SHADER_VERTEX].prefix,
                &ctx->payloads[PIPE_SHADER_FRAGMENT].prefix,
                1, vertex_count, info->instance_count,
                1, 1, 1);

        ctx->payloads[PIPE_SHADER_FRAGMENT].prefix.unknown_draw = draw_flags;

        /* Encode the padded vertex count */

        if (info->instance_count > 1) {
                /* Triangles have non-even vertex counts so they change how
                 * padding works internally */

                bool is_triangle =
                        mode == PIPE_PRIM_TRIANGLES ||
                        mode == PIPE_PRIM_TRIANGLE_STRIP ||
                        mode == PIPE_PRIM_TRIANGLE_FAN;

                struct pan_shift_odd so =
                        panfrost_padded_vertex_count(vertex_count, !is_triangle);

                ctx->payloads[PIPE_SHADER_VERTEX].instance_shift = so.shift;
                ctx->payloads[PIPE_SHADER_FRAGMENT].instance_shift = so.shift;

                ctx->payloads[PIPE_SHADER_VERTEX].instance_odd = so.odd;
                ctx->payloads[PIPE_SHADER_FRAGMENT].instance_odd = so.odd;

                ctx->padded_count = pan_expand_shift_odd(so);
        } else {
                ctx->padded_count = ctx->vertex_count;

                /* Reset instancing state */
                ctx->payloads[PIPE_SHADER_VERTEX].instance_shift = 0;
                ctx->payloads[PIPE_SHADER_VERTEX].instance_odd = 0;
                ctx->payloads[PIPE_SHADER_FRAGMENT].instance_shift = 0;
                ctx->payloads[PIPE_SHADER_FRAGMENT].instance_odd = 0;
        }

        /* Fire off the draw itself */
        panfrost_queue_draw(ctx);
}

/* CSO state */

static void
panfrost_generic_cso_delete(struct pipe_context *pctx, void *hwcso)
{
        free(hwcso);
}

static void *
panfrost_create_rasterizer_state(
        struct pipe_context *pctx,
        const struct pipe_rasterizer_state *cso)
{
        struct panfrost_rasterizer *so = CALLOC_STRUCT(panfrost_rasterizer);

        so->base = *cso;

        /* Bitmask, unknown meaning of the start value. 0x105 on 32-bit T6XX */
        so->tiler_gl_enables = 0x7;

        if (cso->front_ccw)
                so->tiler_gl_enables |= MALI_FRONT_CCW_TOP;

        if (cso->cull_face & PIPE_FACE_FRONT)
                so->tiler_gl_enables |= MALI_CULL_FACE_FRONT;

        if (cso->cull_face & PIPE_FACE_BACK)
                so->tiler_gl_enables |= MALI_CULL_FACE_BACK;

        return so;
}

static void
panfrost_bind_rasterizer_state(
        struct pipe_context *pctx,
        void *hwcso)
{
        struct panfrost_context *ctx = pan_context(pctx);

        /* TODO: Why can't rasterizer be NULL ever? Other drivers are fine.. */
        if (!hwcso)
                return;

        ctx->rasterizer = hwcso;
        ctx->dirty |= PAN_DIRTY_RASTERIZER;

        ctx->fragment_shader_core.depth_units = ctx->rasterizer->base.offset_units;
        ctx->fragment_shader_core.depth_factor = ctx->rasterizer->base.offset_scale;

        /* Gauranteed with the core GL call, so don't expose ARB_polygon_offset */
        assert(ctx->rasterizer->base.offset_clamp == 0.0);

        /* XXX: Which bit is which? Does this maybe allow offseting not-tri? */

        SET_BIT(ctx->fragment_shader_core.unknown2_4, MALI_DEPTH_RANGE_A, ctx->rasterizer->base.offset_tri);
        SET_BIT(ctx->fragment_shader_core.unknown2_4, MALI_DEPTH_RANGE_B, ctx->rasterizer->base.offset_tri);

        /* Point sprites are emulated */

        struct panfrost_shader_state *variant =
                        ctx->shader[PIPE_SHADER_FRAGMENT] ? &ctx->shader[PIPE_SHADER_FRAGMENT]->variants[ctx->shader[PIPE_SHADER_FRAGMENT]->active_variant] : NULL;

        if (ctx->rasterizer->base.sprite_coord_enable || (variant && variant->point_sprite_mask))
                ctx->base.bind_fs_state(&ctx->base, ctx->shader[PIPE_SHADER_FRAGMENT]);
}

static void *
panfrost_create_vertex_elements_state(
        struct pipe_context *pctx,
        unsigned num_elements,
        const struct pipe_vertex_element *elements)
{
        struct panfrost_vertex_state *so = CALLOC_STRUCT(panfrost_vertex_state);

        so->num_elements = num_elements;
        memcpy(so->pipe, elements, sizeof(*elements) * num_elements);

        for (int i = 0; i < num_elements; ++i) {
                so->hw[i].index = i;

                enum pipe_format fmt = elements[i].src_format;
                const struct util_format_description *desc = util_format_description(fmt);
                so->hw[i].unknown1 = 0x2;
                so->hw[i].swizzle = panfrost_get_default_swizzle(desc->nr_channels);

                so->hw[i].format = panfrost_find_format(desc);

                /* The field itself should probably be shifted over */
                so->hw[i].src_offset = elements[i].src_offset;
        }

        return so;
}

static void
panfrost_bind_vertex_elements_state(
        struct pipe_context *pctx,
        void *hwcso)
{
        struct panfrost_context *ctx = pan_context(pctx);

        ctx->vertex = hwcso;
        ctx->dirty |= PAN_DIRTY_VERTEX;
}

static void *
panfrost_create_shader_state(
        struct pipe_context *pctx,
        const struct pipe_shader_state *cso)
{
        struct panfrost_shader_variants *so = CALLOC_STRUCT(panfrost_shader_variants);
        so->base = *cso;

        /* Token deep copy to prevent memory corruption */

        if (cso->type == PIPE_SHADER_IR_TGSI)
                so->base.tokens = tgsi_dup_tokens(so->base.tokens);

        return so;
}

static void
panfrost_delete_shader_state(
        struct pipe_context *pctx,
        void *so)
{
        struct panfrost_shader_variants *cso = (struct panfrost_shader_variants *) so;

        if (cso->base.type == PIPE_SHADER_IR_TGSI) {
                DBG("Deleting TGSI shader leaks duplicated tokens\n");
        }

        free(so);
}

static void *
panfrost_create_sampler_state(
        struct pipe_context *pctx,
        const struct pipe_sampler_state *cso)
{
        struct panfrost_sampler_state *so = CALLOC_STRUCT(panfrost_sampler_state);
        so->base = *cso;

        /* sampler_state corresponds to mali_sampler_descriptor, which we can generate entirely here */

        bool min_nearest = cso->min_img_filter == PIPE_TEX_FILTER_NEAREST;
        bool mag_nearest = cso->mag_img_filter == PIPE_TEX_FILTER_NEAREST;
        bool mip_linear  = cso->min_mip_filter == PIPE_TEX_MIPFILTER_LINEAR;

        unsigned min_filter = min_nearest ? MALI_SAMP_MIN_NEAREST : 0;
        unsigned mag_filter = mag_nearest ? MALI_SAMP_MAG_NEAREST : 0;
        unsigned mip_filter = mip_linear  ?
                (MALI_SAMP_MIP_LINEAR_1 | MALI_SAMP_MIP_LINEAR_2) : 0;
        unsigned normalized = cso->normalized_coords ? MALI_SAMP_NORM_COORDS : 0;

        struct mali_sampler_descriptor sampler_descriptor = {
                .filter_mode = min_filter | mag_filter | mip_filter | normalized,
                .wrap_s = translate_tex_wrap(cso->wrap_s),
                .wrap_t = translate_tex_wrap(cso->wrap_t),
                .wrap_r = translate_tex_wrap(cso->wrap_r),
                .compare_func = panfrost_translate_alt_compare_func(cso->compare_func),
                .border_color = {
                        cso->border_color.f[0],
                        cso->border_color.f[1],
                        cso->border_color.f[2],
                        cso->border_color.f[3]
                },
                .min_lod = FIXED_16(cso->min_lod),
                .max_lod = FIXED_16(cso->max_lod),
                .seamless_cube_map = cso->seamless_cube_map,
        };

        /* If necessary, we disable mipmapping in the sampler descriptor by
         * clamping the LOD as tight as possible (from 0 to epsilon,
         * essentially -- remember these are fixed point numbers, so
         * epsilon=1/256) */

        if (cso->min_mip_filter == PIPE_TEX_MIPFILTER_NONE)
                sampler_descriptor.max_lod = sampler_descriptor.min_lod;

        /* Enforce that there is something in the middle by adding epsilon*/

        if (sampler_descriptor.min_lod == sampler_descriptor.max_lod)
                sampler_descriptor.max_lod++;

        /* Sanity check */
        assert(sampler_descriptor.max_lod > sampler_descriptor.min_lod);

        so->hw = sampler_descriptor;

        return so;
}

static void
panfrost_bind_sampler_states(
        struct pipe_context *pctx,
        enum pipe_shader_type shader,
        unsigned start_slot, unsigned num_sampler,
        void **sampler)
{
        assert(start_slot == 0);

        struct panfrost_context *ctx = pan_context(pctx);

        /* XXX: Should upload, not just copy? */
        ctx->sampler_count[shader] = num_sampler;
        memcpy(ctx->samplers[shader], sampler, num_sampler * sizeof (void *));

        ctx->dirty |= PAN_DIRTY_SAMPLERS;
}

static bool
panfrost_variant_matches(
        struct panfrost_context *ctx,
        struct panfrost_shader_state *variant,
        enum pipe_shader_type type)
{
        struct pipe_rasterizer_state *rasterizer = &ctx->rasterizer->base;
        struct pipe_alpha_state *alpha = &ctx->depth_stencil->alpha;

        bool is_fragment = (type == PIPE_SHADER_FRAGMENT);

        if (is_fragment && (alpha->enabled || variant->alpha_state.enabled)) {
                /* Make sure enable state is at least the same */
                if (alpha->enabled != variant->alpha_state.enabled) {
                        return false;
                }

                /* Check that the contents of the test are the same */
                bool same_func = alpha->func == variant->alpha_state.func;
                bool same_ref = alpha->ref_value == variant->alpha_state.ref_value;

                if (!(same_func && same_ref)) {
                        return false;
                }
        }

        if (is_fragment && rasterizer && (rasterizer->sprite_coord_enable |
                                          variant->point_sprite_mask)) {
                /* Ensure the same varyings are turned to point sprites */
                if (rasterizer->sprite_coord_enable != variant->point_sprite_mask)
                        return false;

                /* Ensure the orientation is correct */
                bool upper_left =
                        rasterizer->sprite_coord_mode ==
                        PIPE_SPRITE_COORD_UPPER_LEFT;

                if (variant->point_sprite_upper_left != upper_left)
                        return false;
        }

        /* Otherwise, we're good to go */
        return true;
}

static void
panfrost_bind_shader_state(
        struct pipe_context *pctx,
        void *hwcso,
        enum pipe_shader_type type)
{
        struct panfrost_context *ctx = pan_context(pctx);

        ctx->shader[type] = hwcso;

        if (type == PIPE_SHADER_FRAGMENT)
                ctx->dirty |= PAN_DIRTY_FS;
        else
                ctx->dirty |= PAN_DIRTY_VS;

        if (!hwcso) return;

        /* Match the appropriate variant */

        signed variant = -1;
        struct panfrost_shader_variants *variants = (struct panfrost_shader_variants *) hwcso;

        for (unsigned i = 0; i < variants->variant_count; ++i) {
                if (panfrost_variant_matches(ctx, &variants->variants[i], type)) {
                        variant = i;
                        break;
                }
        }

        if (variant == -1) {
                /* No variant matched, so create a new one */
                variant = variants->variant_count++;
                assert(variants->variant_count < MAX_SHADER_VARIANTS);

                struct panfrost_shader_state *v =
                                &variants->variants[variant];

                if (type == PIPE_SHADER_FRAGMENT) {
                        v->alpha_state = ctx->depth_stencil->alpha;

                        if (ctx->rasterizer) {
                                v->point_sprite_mask = ctx->rasterizer->base.sprite_coord_enable;
                                v->point_sprite_upper_left =
                                        ctx->rasterizer->base.sprite_coord_mode ==
                                        PIPE_SPRITE_COORD_UPPER_LEFT;
                        }
                }

                variants->variants[variant].tripipe = malloc(sizeof(struct mali_shader_meta));

        }

        /* Select this variant */
        variants->active_variant = variant;

        struct panfrost_shader_state *shader_state = &variants->variants[variant];
        assert(panfrost_variant_matches(ctx, shader_state, type));

        /* We finally have a variant, so compile it */

        if (!shader_state->compiled) {
                panfrost_shader_compile(ctx, shader_state->tripipe,
                              variants->base.type,
                              variants->base.type == PIPE_SHADER_IR_NIR ?
                                      variants->base.ir.nir :
                                      variants->base.tokens,
                                        tgsi_processor_to_shader_stage(type), shader_state);

                shader_state->compiled = true;
        }
}

static void
panfrost_bind_vs_state(struct pipe_context *pctx, void *hwcso)
{
        panfrost_bind_shader_state(pctx, hwcso, PIPE_SHADER_VERTEX);
}

static void
panfrost_bind_fs_state(struct pipe_context *pctx, void *hwcso)
{
        panfrost_bind_shader_state(pctx, hwcso, PIPE_SHADER_FRAGMENT);
}

static void
panfrost_set_vertex_buffers(
        struct pipe_context *pctx,
        unsigned start_slot,
        unsigned num_buffers,
        const struct pipe_vertex_buffer *buffers)
{
        struct panfrost_context *ctx = pan_context(pctx);

        util_set_vertex_buffers_mask(ctx->vertex_buffers, &ctx->vb_mask, buffers, start_slot, num_buffers);
}

static void
panfrost_set_constant_buffer(
        struct pipe_context *pctx,
        enum pipe_shader_type shader, uint index,
        const struct pipe_constant_buffer *buf)
{
        struct panfrost_context *ctx = pan_context(pctx);
        struct panfrost_constant_buffer *pbuf = &ctx->constant_buffer[shader];

        util_copy_constant_buffer(&pbuf->cb[index], buf);

        unsigned mask = (1 << index);

        if (unlikely(!buf)) {
                pbuf->enabled_mask &= ~mask;
                pbuf->dirty_mask &= ~mask;
                return;
        }

        pbuf->enabled_mask |= mask;
        pbuf->dirty_mask |= mask;
}

static void
panfrost_set_stencil_ref(
        struct pipe_context *pctx,
        const struct pipe_stencil_ref *ref)
{
        struct panfrost_context *ctx = pan_context(pctx);
        ctx->stencil_ref = *ref;

        /* Shader core dirty */
        ctx->dirty |= PAN_DIRTY_FS;
}

static enum mali_texture_type
panfrost_translate_texture_type(enum pipe_texture_target t) {
        switch (t)
        {
        case PIPE_BUFFER:
                        case PIPE_TEXTURE_1D:
                                case PIPE_TEXTURE_1D_ARRAY:
                                                return MALI_TEX_1D;

        case PIPE_TEXTURE_2D:
        case PIPE_TEXTURE_2D_ARRAY:
        case PIPE_TEXTURE_RECT:
                return MALI_TEX_2D;

        case PIPE_TEXTURE_3D:
                return MALI_TEX_3D;

        case PIPE_TEXTURE_CUBE:
        case PIPE_TEXTURE_CUBE_ARRAY:
                return MALI_TEX_CUBE;

        default:
                unreachable("Unknown target");
        }
}

static struct pipe_sampler_view *
panfrost_create_sampler_view(
        struct pipe_context *pctx,
        struct pipe_resource *texture,
        const struct pipe_sampler_view *template)
{
        struct panfrost_sampler_view *so = rzalloc(pctx, struct panfrost_sampler_view);
        int bytes_per_pixel = util_format_get_blocksize(texture->format);

        pipe_reference(NULL, &texture->reference);

        struct panfrost_resource *prsrc = (struct panfrost_resource *) texture;
        assert(prsrc->bo);

        so->base = *template;
        so->base.texture = texture;
        so->base.reference.count = 1;
        so->base.context = pctx;

        /* sampler_views correspond to texture descriptors, minus the texture
         * (data) itself. So, we serialise the descriptor here and cache it for
         * later. */

        /* TODO: Detect from format better */
        const struct util_format_description *desc = util_format_description(prsrc->base.format);

        unsigned char user_swizzle[4] = {
                template->swizzle_r,
                template->swizzle_g,
                template->swizzle_b,
                template->swizzle_a
        };

        enum mali_format format = panfrost_find_format(desc);

        /* Check if we need to set a custom stride by computing the "expected"
         * stride and comparing it to what the BO actually wants. Only applies
         * to linear textures, since tiled/compressed textures have strict
         * alignment requirements for their strides as it is */

        unsigned first_level = template->u.tex.first_level;
        unsigned last_level = template->u.tex.last_level;

        if (prsrc->layout == PAN_LINEAR) {
                for (unsigned l = first_level; l <= last_level; ++l) {
                        unsigned actual_stride = prsrc->slices[l].stride;
                        unsigned width = u_minify(texture->width0, l);
                        unsigned comp_stride = width * bytes_per_pixel;

                        if (comp_stride != actual_stride) {
                                so->manual_stride = true;
                                break;
                        }
                }
        }

        /* In the hardware, array_size refers specifically to array textures,
         * whereas in Gallium, it also covers cubemaps */

        unsigned array_size = texture->array_size;

        if (template->target == PIPE_TEXTURE_CUBE) {
                /* TODO: Cubemap arrays */
                assert(array_size == 6);
                array_size /= 6;
        }

        struct mali_texture_descriptor texture_descriptor = {
                .width = MALI_POSITIVE(u_minify(texture->width0, first_level)),
                .height = MALI_POSITIVE(u_minify(texture->height0, first_level)),
                .depth = MALI_POSITIVE(u_minify(texture->depth0, first_level)),
                .array_size = MALI_POSITIVE(array_size),

                /* TODO: Decode */
                .format = {
                        .swizzle = panfrost_translate_swizzle_4(desc->swizzle),
                        .format = format,

                        .srgb = desc->colorspace == UTIL_FORMAT_COLORSPACE_SRGB,
                        .type = panfrost_translate_texture_type(template->target),
                },

                .swizzle = panfrost_translate_swizzle_4(user_swizzle)
        };

        texture_descriptor.nr_mipmap_levels = last_level - first_level;

        so->hw = texture_descriptor;

        return (struct pipe_sampler_view *) so;
}

static void
panfrost_set_sampler_views(
        struct pipe_context *pctx,
        enum pipe_shader_type shader,
        unsigned start_slot, unsigned num_views,
        struct pipe_sampler_view **views)
{
        struct panfrost_context *ctx = pan_context(pctx);

        assert(start_slot == 0);

        unsigned new_nr = 0;
        for (unsigned i = 0; i < num_views; ++i) {
                if (views[i])
                        new_nr = i + 1;
        }

        ctx->sampler_view_count[shader] = new_nr;
        memcpy(ctx->sampler_views[shader], views, num_views * sizeof (void *));

        ctx->dirty |= PAN_DIRTY_TEXTURES;
}

static void
panfrost_sampler_view_destroy(
        struct pipe_context *pctx,
        struct pipe_sampler_view *view)
{
        pipe_resource_reference(&view->texture, NULL);
        ralloc_free(view);
}

static void
panfrost_set_shader_buffers(
        struct pipe_context *pctx,
        enum pipe_shader_type shader,
        unsigned start, unsigned count,
        const struct pipe_shader_buffer *buffers,
        unsigned writable_bitmask)
{
        struct panfrost_context *ctx = pan_context(pctx);

        util_set_shader_buffers_mask(ctx->ssbo[shader], &ctx->ssbo_mask[shader],
                        buffers, start, count);
}

/* Hints that a framebuffer should use AFBC where possible */

static void
panfrost_hint_afbc(
                struct panfrost_screen *screen,
                const struct pipe_framebuffer_state *fb)
{
        /* AFBC implemenation incomplete; hide it */
        if (!(pan_debug & PAN_DBG_AFBC)) return;

        /* Hint AFBC to the resources bound to each color buffer */

        for (unsigned i = 0; i < fb->nr_cbufs; ++i) {
                struct pipe_surface *surf = fb->cbufs[i];
                struct panfrost_resource *rsrc = pan_resource(surf->texture);
                panfrost_resource_hint_layout(screen, rsrc, PAN_AFBC, 1);
        }

        /* Also hint it to the depth buffer */

        if (fb->zsbuf) {
                struct panfrost_resource *rsrc = pan_resource(fb->zsbuf->texture);
                panfrost_resource_hint_layout(screen, rsrc, PAN_AFBC, 1);
        }
}

static void
panfrost_set_framebuffer_state(struct pipe_context *pctx,
                               const struct pipe_framebuffer_state *fb)
{
        struct panfrost_context *ctx = pan_context(pctx);

        /* Flush when switching framebuffers, but not if the framebuffer
         * state is being restored by u_blitter
         */

        struct panfrost_job *job = panfrost_get_job_for_fbo(ctx);
        bool is_scanout = panfrost_is_scanout(ctx);
        bool has_draws = job->last_job.gpu;

        /* Bail out early when the current and new states are the same. */
        if (util_framebuffer_state_equal(&ctx->pipe_framebuffer, fb))
                return;

        /* The wallpaper logic sets a new FB state before doing the blit and
         * restore the old one when it's done. Those FB states are reported to
         * be different because the surface they are pointing to are different,
         * but those surfaces actually point to the same cbufs/zbufs. In that
         * case we definitely don't want new FB descs to be emitted/attached
         * since the job is expected to be flushed just after the blit is done,
         * so let's just copy the new state and return here.
         */
        if (ctx->wallpaper_batch) {
                util_copy_framebuffer_state(&ctx->pipe_framebuffer, fb);
                return;
        }

        if (!is_scanout || has_draws)
                panfrost_flush(pctx, NULL, PIPE_FLUSH_END_OF_FRAME);
        else
                assert(!ctx->payloads[PIPE_SHADER_VERTEX].postfix.framebuffer &&
                       !ctx->payloads[PIPE_SHADER_FRAGMENT].postfix.framebuffer);

        /* Invalidate the FBO job cache since we've just been assigned a new
         * FB state.
         */
        ctx->job = NULL;

        util_copy_framebuffer_state(&ctx->pipe_framebuffer, fb);

        /* Given that we're rendering, we'd love to have compression */
        struct panfrost_screen *screen = pan_screen(ctx->base.screen);

        panfrost_hint_afbc(screen, &ctx->pipe_framebuffer);
        for (unsigned i = 0; i < PIPE_SHADER_TYPES; ++i)
                ctx->payloads[i].postfix.framebuffer = 0;
}

static void *
panfrost_create_depth_stencil_state(struct pipe_context *pipe,
                                    const struct pipe_depth_stencil_alpha_state *depth_stencil)
{
        return mem_dup(depth_stencil, sizeof(*depth_stencil));
}

static void
panfrost_bind_depth_stencil_state(struct pipe_context *pipe,
                                  void *cso)
{
        struct panfrost_context *ctx = pan_context(pipe);
        struct pipe_depth_stencil_alpha_state *depth_stencil = cso;
        ctx->depth_stencil = depth_stencil;

        if (!depth_stencil)
                return;

        /* Alpha does not exist in the hardware (it's not in ES3), so it's
         * emulated in the fragment shader */

        if (depth_stencil->alpha.enabled) {
                /* We need to trigger a new shader (maybe) */
                ctx->base.bind_fs_state(&ctx->base, ctx->shader[PIPE_SHADER_FRAGMENT]);
        }

        /* Stencil state */
        SET_BIT(ctx->fragment_shader_core.unknown2_4, MALI_STENCIL_TEST, depth_stencil->stencil[0].enabled);

        panfrost_make_stencil_state(&depth_stencil->stencil[0], &ctx->fragment_shader_core.stencil_front);
        ctx->fragment_shader_core.stencil_mask_front = depth_stencil->stencil[0].writemask;

        /* If back-stencil is not enabled, use the front values */
        bool back_enab = ctx->depth_stencil->stencil[1].enabled;
        unsigned back_index = back_enab ? 1 : 0;

        panfrost_make_stencil_state(&depth_stencil->stencil[back_index], &ctx->fragment_shader_core.stencil_back);
        ctx->fragment_shader_core.stencil_mask_back = depth_stencil->stencil[back_index].writemask;

        /* Depth state (TODO: Refactor) */
        SET_BIT(ctx->fragment_shader_core.unknown2_3, MALI_DEPTH_TEST, depth_stencil->depth.enabled);

        int func = depth_stencil->depth.enabled ? depth_stencil->depth.func : PIPE_FUNC_ALWAYS;

        ctx->fragment_shader_core.unknown2_3 &= ~MALI_DEPTH_FUNC_MASK;
        ctx->fragment_shader_core.unknown2_3 |= MALI_DEPTH_FUNC(panfrost_translate_compare_func(func));

        /* Bounds test not implemented */
        assert(!depth_stencil->depth.bounds_test);

        ctx->dirty |= PAN_DIRTY_FS;
}

static void
panfrost_delete_depth_stencil_state(struct pipe_context *pipe, void *depth)
{
        free( depth );
}

static void
panfrost_set_sample_mask(struct pipe_context *pipe,
                         unsigned sample_mask)
{
}

static void
panfrost_set_clip_state(struct pipe_context *pipe,
                        const struct pipe_clip_state *clip)
{
        //struct panfrost_context *panfrost = pan_context(pipe);
}

static void
panfrost_set_viewport_states(struct pipe_context *pipe,
                             unsigned start_slot,
                             unsigned num_viewports,
                             const struct pipe_viewport_state *viewports)
{
        struct panfrost_context *ctx = pan_context(pipe);

        assert(start_slot == 0);
        assert(num_viewports == 1);

        ctx->pipe_viewport = *viewports;
}

static void
panfrost_set_scissor_states(struct pipe_context *pipe,
                            unsigned start_slot,
                            unsigned num_scissors,
                            const struct pipe_scissor_state *scissors)
{
        struct panfrost_context *ctx = pan_context(pipe);

        assert(start_slot == 0);
        assert(num_scissors == 1);

        ctx->scissor = *scissors;
}

static void
panfrost_set_polygon_stipple(struct pipe_context *pipe,
                             const struct pipe_poly_stipple *stipple)
{
        //struct panfrost_context *panfrost = pan_context(pipe);
}

static void
panfrost_set_active_query_state(struct pipe_context *pipe,
                                bool enable)
{
        //struct panfrost_context *panfrost = pan_context(pipe);
}

static void
panfrost_destroy(struct pipe_context *pipe)
{
        struct panfrost_context *panfrost = pan_context(pipe);
        struct panfrost_screen *screen = pan_screen(pipe->screen);

        if (panfrost->blitter)
                util_blitter_destroy(panfrost->blitter);

        if (panfrost->blitter_wallpaper)
                util_blitter_destroy(panfrost->blitter_wallpaper);

        panfrost_drm_free_slab(screen, &panfrost->scratchpad);
        panfrost_drm_free_slab(screen, &panfrost->shaders);
        panfrost_drm_free_slab(screen, &panfrost->tiler_heap);
        panfrost_drm_free_slab(screen, &panfrost->tiler_dummy);

        ralloc_free(pipe);
}

static struct pipe_query *
panfrost_create_query(struct pipe_context *pipe,
                      unsigned type,
                      unsigned index)
{
        struct panfrost_query *q = rzalloc(pipe, struct panfrost_query);

        q->type = type;
        q->index = index;

        return (struct pipe_query *) q;
}

static void
panfrost_destroy_query(struct pipe_context *pipe, struct pipe_query *q)
{
        ralloc_free(q);
}

static bool
panfrost_begin_query(struct pipe_context *pipe, struct pipe_query *q)
{
        struct panfrost_context *ctx = pan_context(pipe);
        struct panfrost_query *query = (struct panfrost_query *) q;

        switch (query->type) {
        case PIPE_QUERY_OCCLUSION_COUNTER:
        case PIPE_QUERY_OCCLUSION_PREDICATE:
        case PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE: {
                /* Allocate a word for the query results to be stored */
                query->transfer = panfrost_allocate_transient(ctx, sizeof(unsigned));

                ctx->occlusion_query = query;

                break;
        }

        default:
                DBG("Skipping query %d\n", query->type);
                break;
        }

        return true;
}

static bool
panfrost_end_query(struct pipe_context *pipe, struct pipe_query *q)
{
        struct panfrost_context *ctx = pan_context(pipe);
        ctx->occlusion_query = NULL;
        return true;
}

static bool
panfrost_get_query_result(struct pipe_context *pipe,
                          struct pipe_query *q,
                          bool wait,
                          union pipe_query_result *vresult)
{
        /* STUB */
        struct panfrost_query *query = (struct panfrost_query *) q;

        /* We need to flush out the jobs to actually run the counter, TODO
         * check wait, TODO wallpaper after if needed */

        panfrost_flush(pipe, NULL, PIPE_FLUSH_END_OF_FRAME);

        switch (query->type) {
        case PIPE_QUERY_OCCLUSION_COUNTER:
        case PIPE_QUERY_OCCLUSION_PREDICATE:
        case PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE: {
                /* Read back the query results */
                unsigned *result = (unsigned *) query->transfer.cpu;
                unsigned passed = *result;

                if (query->type == PIPE_QUERY_OCCLUSION_COUNTER) {
                        vresult->u64 = passed;
                } else {
                        vresult->b = !!passed;
                }

                break;
        }
        default:
                DBG("Skipped query get %d\n", query->type);
                break;
        }

        return true;
}

static struct pipe_stream_output_target *
panfrost_create_stream_output_target(struct pipe_context *pctx,
                                     struct pipe_resource *prsc,
                                     unsigned buffer_offset,
                                     unsigned buffer_size)
{
        struct pipe_stream_output_target *target;

        target = rzalloc(pctx, struct pipe_stream_output_target);

        if (!target)
                return NULL;

        pipe_reference_init(&target->reference, 1);
        pipe_resource_reference(&target->buffer, prsc);

        target->context = pctx;
        target->buffer_offset = buffer_offset;
        target->buffer_size = buffer_size;

        return target;
}

static void
panfrost_stream_output_target_destroy(struct pipe_context *pctx,
                                      struct pipe_stream_output_target *target)
{
        pipe_resource_reference(&target->buffer, NULL);
        ralloc_free(target);
}

static void
panfrost_set_stream_output_targets(struct pipe_context *pctx,
                                   unsigned num_targets,
                                   struct pipe_stream_output_target **targets,
                                   const unsigned *offsets)
{
        /* STUB */
}

static void
panfrost_setup_hardware(struct panfrost_context *ctx)
{
        struct pipe_context *gallium = (struct pipe_context *) ctx;
        struct panfrost_screen *screen = pan_screen(gallium->screen);

        panfrost_drm_allocate_slab(screen, &ctx->scratchpad, 64*4, false, 0, 0, 0);
        panfrost_drm_allocate_slab(screen, &ctx->shaders, 4096, true, PAN_ALLOCATE_EXECUTE, 0, 0);
        panfrost_drm_allocate_slab(screen, &ctx->tiler_heap, 4096, false, PAN_ALLOCATE_INVISIBLE | PAN_ALLOCATE_GROWABLE, 1, 128);
        panfrost_drm_allocate_slab(screen, &ctx->tiler_dummy, 1, false, PAN_ALLOCATE_INVISIBLE, 0, 0);
}

/* New context creation, which also does hardware initialisation since I don't
 * know the better way to structure this :smirk: */

struct pipe_context *
panfrost_create_context(struct pipe_screen *screen, void *priv, unsigned flags)
{
        struct panfrost_context *ctx = rzalloc(screen, struct panfrost_context);
        struct panfrost_screen *pscreen = pan_screen(screen);
        memset(ctx, 0, sizeof(*ctx));
        struct pipe_context *gallium = (struct pipe_context *) ctx;

        ctx->is_t6xx = pscreen->gpu_id < 0x0700; /* Literally, "earlier than T700" */

        gallium->screen = screen;

        gallium->destroy = panfrost_destroy;

        gallium->set_framebuffer_state = panfrost_set_framebuffer_state;

        gallium->flush = panfrost_flush;
        gallium->clear = panfrost_clear;
        gallium->draw_vbo = panfrost_draw_vbo;

        gallium->set_vertex_buffers = panfrost_set_vertex_buffers;
        gallium->set_constant_buffer = panfrost_set_constant_buffer;
        gallium->set_shader_buffers = panfrost_set_shader_buffers;

        gallium->set_stencil_ref = panfrost_set_stencil_ref;

        gallium->create_sampler_view = panfrost_create_sampler_view;
        gallium->set_sampler_views = panfrost_set_sampler_views;
        gallium->sampler_view_destroy = panfrost_sampler_view_destroy;

        gallium->create_rasterizer_state = panfrost_create_rasterizer_state;
        gallium->bind_rasterizer_state = panfrost_bind_rasterizer_state;
        gallium->delete_rasterizer_state = panfrost_generic_cso_delete;

        gallium->create_vertex_elements_state = panfrost_create_vertex_elements_state;
        gallium->bind_vertex_elements_state = panfrost_bind_vertex_elements_state;
        gallium->delete_vertex_elements_state = panfrost_generic_cso_delete;

        gallium->create_fs_state = panfrost_create_shader_state;
        gallium->delete_fs_state = panfrost_delete_shader_state;
        gallium->bind_fs_state = panfrost_bind_fs_state;

        gallium->create_vs_state = panfrost_create_shader_state;
        gallium->delete_vs_state = panfrost_delete_shader_state;
        gallium->bind_vs_state = panfrost_bind_vs_state;

        gallium->create_sampler_state = panfrost_create_sampler_state;
        gallium->delete_sampler_state = panfrost_generic_cso_delete;
        gallium->bind_sampler_states = panfrost_bind_sampler_states;

        gallium->create_depth_stencil_alpha_state = panfrost_create_depth_stencil_state;
        gallium->bind_depth_stencil_alpha_state   = panfrost_bind_depth_stencil_state;
        gallium->delete_depth_stencil_alpha_state = panfrost_delete_depth_stencil_state;

        gallium->set_sample_mask = panfrost_set_sample_mask;

        gallium->set_clip_state = panfrost_set_clip_state;
        gallium->set_viewport_states = panfrost_set_viewport_states;
        gallium->set_scissor_states = panfrost_set_scissor_states;
        gallium->set_polygon_stipple = panfrost_set_polygon_stipple;
        gallium->set_active_query_state = panfrost_set_active_query_state;

        gallium->create_query = panfrost_create_query;
        gallium->destroy_query = panfrost_destroy_query;
        gallium->begin_query = panfrost_begin_query;
        gallium->end_query = panfrost_end_query;
        gallium->get_query_result = panfrost_get_query_result;

        gallium->create_stream_output_target = panfrost_create_stream_output_target;
        gallium->stream_output_target_destroy = panfrost_stream_output_target_destroy;
        gallium->set_stream_output_targets = panfrost_set_stream_output_targets;

        panfrost_resource_context_init(gallium);
        panfrost_blend_context_init(gallium);
        panfrost_compute_context_init(gallium);

        panfrost_drm_init_context(ctx);

        panfrost_setup_hardware(ctx);

        /* XXX: leaks */
        gallium->stream_uploader = u_upload_create_default(gallium);
        gallium->const_uploader = gallium->stream_uploader;
        assert(gallium->stream_uploader);

        /* Midgard supports ES modes, plus QUADS/QUAD_STRIPS/POLYGON */
        ctx->draw_modes = (1 << (PIPE_PRIM_POLYGON + 1)) - 1;

        ctx->primconvert = util_primconvert_create(gallium, ctx->draw_modes);

        ctx->blitter = util_blitter_create(gallium);
        ctx->blitter_wallpaper = util_blitter_create(gallium);

        assert(ctx->blitter);
        assert(ctx->blitter_wallpaper);

        /* Prepare for render! */

        panfrost_job_init(ctx);
        panfrost_emit_vertex_payload(ctx);
        panfrost_emit_tiler_payload(ctx);
        panfrost_invalidate_frame(ctx);
        panfrost_default_shader_backend(ctx);

        return gallium;
}
