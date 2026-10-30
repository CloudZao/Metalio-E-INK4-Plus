#include "epd_content_policy.h"
#include "reader/image_tone.h"
#include "src/draw/lv_draw_private.h"
#include "src/draw/sw/blend/lv_draw_sw_blend_private.h"
#include "src/draw/sw/blend/lv_draw_sw_blend_to_l8.h"
#include "src/misc/lv_area_private.h"
#include <algorithm>
#include <vector>

namespace {
bool EpdContentUsesGray(lv_obj_t* object);
EpdContentRole ContentRole(lv_obj_t* object);
struct Entry { lv_obj_t* object; EpdContentRole role; bool has_gray; };
std::vector<Entry> entries;
// Central policy switches: fast list/reading entry preferences can be added
// here without changing decoding, storage, or any individual image renderer.
bool GrayRole(EpdContentRole role) {
    switch(role) {
        case EpdContentRole::Monochrome:
        case EpdContentRole::FastMonochrome: return false;
        default: return true;
    }
}
bool ImageHasGray(lv_obj_t* object) {
    if(!lv_obj_check_type(object,&lv_image_class)) return true;
    const void* source=lv_image_get_src(object);
    if(!source || lv_image_src_get_type(source)!=LV_IMAGE_SRC_VARIABLE) return true;
    const auto* image=static_cast<const lv_image_dsc_t*>(source);
    if(image->header.cf!=LV_COLOR_FORMAT_L8) return true;
    if(!image->data || image->header.stride<image->header.w ||
       image->data_size<size_t(image->header.stride)*image->header.h) return false;
    for(unsigned y=0;y<image->header.h;++y) for(unsigned x=0;x<image->header.w;++x) {
        const auto pixel=image->data[size_t(y)*image->header.stride+x];
        if(pixel!=0 && pixel!=255) return true;
    }
    return false;
}
void Deleted(lv_event_t* event) {
    auto* object=static_cast<lv_obj_t*>(lv_event_get_target(event));
    entries.erase(std::remove_if(entries.begin(),entries.end(),
        [object](const Entry& e) { return e.object==object; }),entries.end());
}
bool OpaqueCover(lv_obj_t* object,const lv_area_t& area) {
    if(lv_obj_has_flag(object,LV_OBJ_FLAG_HIDDEN) ||
       lv_obj_get_style_opa(object,LV_PART_MAIN)!=LV_OPA_COVER ||
       lv_obj_get_style_bg_opa(object,LV_PART_MAIN)!=LV_OPA_COVER ||
       lv_obj_get_style_radius(object,LV_PART_MAIN)!=0 ||
       lv_obj_get_style_bitmap_mask_src(object,LV_PART_MAIN)) return false;
    lv_area_t box; lv_obj_get_coords(object,&box);
    return lv_area_is_in(&area,&box,0);
}
bool CoveredByChildren(lv_obj_t* parent,uint32_t first,const lv_area_t& area) {
    for(uint32_t i=first;i<lv_obj_get_child_count(parent);++i)
        if(OpaqueCover(lv_obj_get_child(parent,i),area)) return true;
    return false;
}
bool Visible(lv_obj_t* object,lv_display_t* display) {
    if(!lv_obj_is_visible(object)) return false;
    auto* screen=lv_obj_get_screen(object);
    if(screen!=lv_display_get_screen_active(display) &&
       screen!=lv_display_get_layer_top(display) && screen!=lv_display_get_layer_sys(display)) return false;
    lv_area_t area; lv_obj_get_coords(object,&area);
    for(auto* node=object;node;node=lv_obj_get_parent(node)) {
        if(lv_obj_get_style_opa(node,LV_PART_MAIN)<=LV_OPA_MIN) return false;
        auto* parent=lv_obj_get_parent(node);
        if(!parent) break;
        if(!lv_obj_has_flag(parent,LV_OBJ_FLAG_OVERFLOW_VISIBLE)) {
            lv_area_t clip; lv_obj_get_coords(parent,&clip);
            if(!lv_area_intersect(&area,&area,&clip)) return false;
        }
    }
    for(auto* node=object;lv_obj_get_parent(node);node=lv_obj_get_parent(node)) {
        auto* parent=lv_obj_get_parent(node);
        if(CoveredByChildren(parent,lv_obj_get_index(node)+1,area)) return false;
    }
    if(screen==lv_display_get_screen_active(display) &&
       CoveredByChildren(lv_display_get_layer_top(display),0,area)) return false;
    if(screen!=lv_display_get_layer_sys(display) &&
       CoveredByChildren(lv_display_get_layer_sys(display),0,area)) return false;
    return true;
}

// Native L8 blending, with source-side binary conversion for untagged UI.
// No shadow framebuffer or post-flush restoration: LVGL still owns every layer,
// clip and alpha operation. Layer tasks already contain individually styled UI.
void Blend(lv_draw_task_t* task,const lv_draw_sw_blend_dsc_t* dsc) {
    if(dsc->opa<=LV_OPA_MIN || (dsc->mask_buf && dsc->mask_res==LV_DRAW_SW_MASK_RES_TRANSP)) return;
    lv_area_t area;
    if(!lv_area_intersect(&area,dsc->blend_area,&task->clip_area)) return;
    if(dsc->src_buf && !lv_area_intersect(&area,&area,dsc->src_area)) return;
    if(dsc->mask_buf && dsc->mask_area && !lv_area_intersect(&area,&area,dsc->mask_area)) return;
    auto* layer=task->target_layer;
    const auto* base=static_cast<const lv_draw_dsc_base_t*>(task->draw_dsc);
    const auto role=ContentRole(base->obj);
    const bool gray=task->type==LV_DRAW_TASK_TYPE_LAYER || GrayRole(role);
    const bool darken=gray && task->type==LV_DRAW_TASK_TYPE_IMAGE &&
        role!=EpdContentRole::ReaderAntialias && role!=EpdContentRole::Shelf;
    const bool label=task->type==LV_DRAW_TASK_TYPE_LABEL;
    lv_area_t relative=area; lv_area_move(&relative,-layer->buf_area.x1,-layer->buf_area.y1);
    const int width=lv_area_get_width(&area),height=lv_area_get_height(&area);
    auto* dest=static_cast<uint8_t*>(lv_draw_layer_go_to_xy(layer,relative.x1,relative.y1));
    const uint8_t* mask=dsc->mask_res==LV_DRAW_SW_MASK_RES_FULL_COVER ? nullptr : dsc->mask_buf;
    int mask_stride=0;
    if(mask) {
        mask_stride=dsc->mask_stride ? dsc->mask_stride : lv_area_get_width(dsc->mask_area);
        mask+=size_t(area.y1-dsc->mask_area->y1)*mask_stride+area.x1-dsc->mask_area->x1;
    }
    const int dest_stride=layer->draw_buf->header.stride;
    if(!dsc->src_buf && gray) {
        lv_draw_sw_blend_fill_dsc_t fill{};
        fill.dest_buf=dest; fill.dest_w=width; fill.dest_h=height; fill.dest_stride=dest_stride;
        fill.color=dsc->color; fill.opa=dsc->opa; fill.relative_area=relative;
        fill.mask_buf=mask; fill.mask_stride=mask_stride;
        lv_draw_sw_blend_color_to_l8(&fill); return;
    }
    if(!dsc->src_buf && !mask) {
        // White UI backgrounds dominate gray-content pages. Preserve the
        // native bulk-fill path instead of converting a row at a time.
        lv_draw_sw_blend_fill_dsc_t fill{};
        fill.dest_buf=dest;fill.dest_w=width;fill.dest_h=height;fill.dest_stride=dest_stride;
        fill.color=lv_color_luminance(dsc->color)>16 ? lv_color_white() : lv_color_black();
        fill.opa=LV_OPA_COVER;fill.relative_area=relative;
        lv_draw_sw_blend_color_to_l8(&fill);return;
    }
    if(dsc->src_buf && gray && !darken) {
        lv_draw_sw_blend_image_dsc_t image{};
        image.dest_buf=dest; image.dest_w=width; image.dest_h=height; image.dest_stride=dest_stride;
        image.opa=task->type==LV_DRAW_TASK_TYPE_LAYER ? static_cast<lv_opa_t>(LV_OPA_COVER) : dsc->opa; image.blend_mode=dsc->blend_mode;
        image.src_stride=dsc->src_stride; image.src_color_format=dsc->src_color_format;
        image.src_buf=static_cast<const uint8_t*>(dsc->src_buf)+
            size_t(area.y1-dsc->src_area->y1)*dsc->src_stride+
            ((area.x1-dsc->src_area->x1)*lv_color_format_get_bpp(dsc->src_color_format)>>3);
        image.src_area=*dsc->src_area; lv_area_move(&image.src_area,-layer->buf_area.x1,-layer->buf_area.y1);
        image.relative_area=relative; image.mask_buf=mask; image.mask_stride=mask_stride;
        lv_draw_sw_blend_image_to_l8(&image); return;
    }
    // One reusable row, bounded by the layer width. No source mutation/cache
    // poisoning: the same resource may be reused later as actual gray content.
    static std::vector<uint8_t> row,mask_row;
    row.resize(size_t(width)*2); mask_row.resize(width);
    const auto* source=static_cast<const uint8_t*>(dsc->src_buf);
    for(int y=0;y<height;++y) {
        for(int x=0;x<width;++x) {
            const unsigned alpha=mask ? mask[size_t(y)*mask_stride+x] : 255;
            mask_row[x]=darken ? alpha : (alpha>(label ? 0u : 127u) ? 255 : 0);
        }
        if(!source) {
            lv_draw_sw_blend_fill_dsc_t fill{};
            fill.dest_buf=dest+size_t(y)*dest_stride; fill.dest_w=width; fill.dest_h=1;
            fill.dest_stride=dest_stride; fill.opa=LV_OPA_COVER;
            fill.color=lv_color_luminance(dsc->color)>16 ? lv_color_white() : lv_color_black();
            fill.mask_buf=mask_row.data(); fill.mask_stride=width;
            lv_draw_sw_blend_color_to_l8(&fill); continue;
        }
        const int sx=area.x1-dsc->src_area->x1,sy=area.y1-dsc->src_area->y1+y;
        const auto* input=source+size_t(sy)*dsc->src_stride;
        for(int x=0;x<width;++x) {
            const int i=sx+x;
            unsigned lum=255,alpha=255,r=0,g=0,b=0;
            switch(dsc->src_color_format) {
                case LV_COLOR_FORMAT_L8: lum=input[i]; break;
                case LV_COLOR_FORMAT_AL88: lum=input[i*2]; alpha=input[i*2+1]; break;
                case LV_COLOR_FORMAT_I1: lum=((input[i/8]>>(7-i%8))&1)*255; break;
                case LV_COLOR_FORMAT_RGB565:
                case LV_COLOR_FORMAT_RGB565_SWAPPED: {
                    unsigned p=dsc->src_color_format==LV_COLOR_FORMAT_RGB565
                        ? input[i*2]|(unsigned(input[i*2+1])<<8)
                        : input[i*2+1]|(unsigned(input[i*2])<<8);
                    r=((p>>11)&31)*255/31; g=((p>>5)&63)*255/63; b=(p&31)*255/31;
                    lum=(r*77+g*150+b*29)>>8; break;
                }
                case LV_COLOR_FORMAT_RGB888: b=input[i*3];g=input[i*3+1];r=input[i*3+2];lum=(r*77+g*150+b*29)>>8;break;
                case LV_COLOR_FORMAT_ARGB8888:
                case LV_COLOR_FORMAT_ARGB8888_PREMULTIPLIED:
                case LV_COLOR_FORMAT_XRGB8888:
                    b=input[i*4];g=input[i*4+1];r=input[i*4+2];lum=(r*77+g*150+b*29)>>8;
                    if(dsc->src_color_format!=LV_COLOR_FORMAT_XRGB8888) alpha=input[i*4+3];
                    if(dsc->src_color_format==LV_COLOR_FORMAT_ARGB8888_PREMULTIPLIED && alpha)
                        lum=std::min(255u,(lum*255+alpha/2)/alpha);
                    break;
                case LV_COLOR_FORMAT_A8:
                    lum=lv_color_luminance(dsc->color);alpha=input[i];break;
                default: LV_LOG_WARN("Unsupported L8 source format"); break;
            }
            // 图片只转换本次绘制源像素，原图、透明度和蒙版保持不变。
            row[x*2]=darken ? reader::DarkenImagePixel(lum) : (lum>16 ? 255 : 0);
            row[x*2+1]=darken ? alpha : (alpha>127 ? 255 : 0);
        }
        lv_draw_sw_blend_image_dsc_t image{};
        image.dest_buf=dest+size_t(y)*dest_stride; image.dest_w=width; image.dest_h=1;
        image.dest_stride=dest_stride; image.opa=darken ? dsc->opa : static_cast<lv_opa_t>(LV_OPA_COVER);
        image.blend_mode=darken ? dsc->blend_mode : LV_BLEND_MODE_NORMAL;
        image.src_buf=row.data(); image.src_stride=width*2; image.src_color_format=LV_COLOR_FORMAT_AL88;
        image.mask_buf=mask ? mask_row.data() : nullptr; image.mask_stride=width;
        lv_draw_sw_blend_image_to_l8(&image);
    }
}
} // namespace

void EpdSetContentRole(lv_obj_t* object,EpdContentRole role) {
    if(!object) return;
    for(auto& entry:entries) if(entry.object==object) { entry.role=role; entry.has_gray=GrayRole(role) && ImageHasGray(object); lv_obj_invalidate(object); return; }
    entries.push_back({object,role,GrayRole(role) && ImageHasGray(object)});
    lv_obj_add_event_cb(object,Deleted,LV_EVENT_DELETE,nullptr);
    lv_obj_invalidate(object);
}
namespace {
EpdContentRole ContentRole(lv_obj_t* object) {
    bool found=false;
    auto role=EpdContentRole::Monochrome;
    for(auto* node=object;node;node=lv_obj_get_parent(node))
        for(const auto& entry:entries) if(entry.object==node) {
            if(entry.role==EpdContentRole::FastMonochrome) return entry.role;
            if(!found) { role=entry.role;found=true; }
            break;
        }
    return role;
}
bool EpdContentUsesGray(lv_obj_t* object) {
    return GrayRole(ContentRole(object));
}
} // namespace

bool EpdVisibleContentNeedsGray(lv_display_t* display) {
    for(const auto& entry:entries)
        if(entry.has_gray && EpdContentUsesGray(entry.object) && Visible(entry.object,display)) return true;
    return false;
}
bool EpdContentBlendInit() {
    lv_draw_sw_custom_blend_handler_t handler{LV_COLOR_FORMAT_L8,Blend};
    return lv_draw_sw_register_blend_handler(&handler);
}
