// Native LVGL 8 regression: outer pager lock must not block child controls.
#include <lvgl.h>
#include <assert.h>
#include <stdio.h>
static int x,y,pressed;
static void readTouch(lv_indev_drv_t *d,lv_indev_data_t *v) {
 (void)d;v->point.x=x;v->point.y=y;v->state=pressed?LV_INDEV_STATE_PR:LV_INDEV_STATE_REL;
}
static void flush(lv_disp_drv_t *d,const lv_area_t *a,lv_color_t *p) {(void)a;(void)p;lv_disp_flush_ready(d);}
static void tick(int n) {for(int i=0;i<n;++i){lv_tick_inc(20);lv_timer_handler();}}
static void drag(int x1,int y1,int x2,int y2) {
 x=x1;y=y1;pressed=1;tick(3);
 for(int i=1;i<=15;++i){x=x1+(x2-x1)*i/15;y=y1+(y2-y1)*i/15;tick(1);}
 pressed=0;tick(40);
}
int main(void) {
 lv_init();static lv_color_t buf[480*20];static lv_disp_draw_buf_t db;
 lv_disp_draw_buf_init(&db,buf,NULL,480*20);
 lv_disp_drv_t dd;lv_disp_drv_init(&dd);dd.hor_res=480;dd.ver_res=480;dd.draw_buf=&db;dd.flush_cb=flush;lv_disp_drv_register(&dd);
 lv_indev_drv_t id;lv_indev_drv_init(&id);id.type=LV_INDEV_TYPE_POINTER;id.read_cb=readTouch;lv_indev_drv_register(&id);
 lv_obj_t *pager=lv_tileview_create(lv_scr_act());lv_obj_set_size(pager,480,390);
 lv_obj_t *a=lv_tileview_add_tile(pager,0,0,LV_DIR_RIGHT);
 lv_obj_t *b=lv_tileview_add_tile(pager,1,0,LV_DIR_HOR);
 lv_tileview_add_tile(pager,2,0,LV_DIR_LEFT);tick(5);
 drag(390,200,70,200);assert(lv_tileview_get_tile_act(pager)==b);
 lv_obj_clear_flag(pager,LV_OBJ_FLAG_SCROLLABLE);
 drag(390,200,70,200);assert(lv_tileview_get_tile_act(pager)==b);
 drag(70,200,390,200);assert(lv_tileview_get_tile_act(pager)==b);
 lv_obj_t *slider=lv_slider_create(b);lv_obj_set_pos(slider,60,120);lv_obj_set_size(slider,350,16);
 lv_slider_set_range(slider,5,50);lv_slider_set_value(slider,5,LV_ANIM_OFF);
 lv_obj_clear_flag(slider,LV_OBJ_FLAG_SCROLL_CHAIN_HOR);tick(5);
 drag(65,128,400,128);assert(lv_slider_get_value(slider)>40);assert(lv_tileview_get_tile_act(pager)==b);
 lv_obj_t *list=lv_obj_create(b);lv_obj_set_pos(list,20,180);lv_obj_set_size(list,420,190);
 lv_obj_set_scroll_dir(list,LV_DIR_VER);lv_obj_clear_flag(list,LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
 lv_obj_t *content=lv_obj_create(list);lv_obj_set_size(content,300,600);tick(5);
 drag(200,340,200,220);assert(lv_obj_get_scroll_y(list)>20);assert(lv_tileview_get_tile_act(pager)==b);
 lv_obj_del(slider);lv_obj_del(list);lv_obj_add_flag(pager,LV_OBJ_FLAG_SCROLLABLE);tick(5);
 drag(70,200,390,200);assert(lv_tileview_get_tile_act(pager)==a);
 puts("Pager lock, slider, vertical scroll and return-to-main unlock passed");
}
