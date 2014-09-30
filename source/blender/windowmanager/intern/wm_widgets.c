/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version. 
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2007 Blender Foundation but based 
 * on ghostwinlay.c (C) 2001-2002 by NaN Holding BV
 * All rights reserved.
 *
 * Contributor(s): Blender Foundation, 2008
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/windowmanager/intern/wm_widgets.c
 *  \ingroup wm
 *
 * Window management, widget API.
 */

#include <stdlib.h>
#include <string.h>

#include "DNA_screen_types.h"
#include "DNA_scene_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_view3d_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_math.h"

#include "BKE_context.h"
#include "BKE_idprop.h"
#include "BKE_global.h"
#include "BKE_main.h"
#include "BKE_report.h"
#include "BKE_scene.h"
#include "BKE_screen.h"


#include "WM_api.h"
#include "WM_types.h"
#include "wm.h"
#include "wm_window.h"
#include "wm_event_system.h"
#include "wm_event_types.h"
#include "wm_draw.h"

#include "GL/glew.h"
#include "GPU_select.h"

#ifndef NDEBUG
#  include "RNA_enum_types.h"
#endif

typedef struct wmWidgetMap {
	struct wmWidgetMap *next, *prev;
	
	ListBase widgets;
	short spaceid, regionid;
	char idname[KMAP_MAX_NAME];
	
} wmWidgetMap;

/* store all widgetboxmaps here. Anyone who wants to register a widget for a certain 
 * area type can query the widgetbox to do so */
static ListBase widgetmaps = {NULL, NULL};


wmWidget *WM_widget_new(bool (*poll)(const struct bContext *C, struct wmWidget *customdata),
                        void (*draw)(const struct bContext *C, struct wmWidget *customdata),
                        void (*render_3d_intersection)(const struct bContext *C, struct wmWidget *customdata),
						int  (*intersect)(struct bContext *C, const struct wmEvent *event, struct wmWidget *customdata),
                        int  (*handler)(struct bContext *C, const struct wmEvent *event, struct wmWidget *customdata),
                        void *customdata, bool free_data, bool requires_ogl)
{
	wmWidget *widget = MEM_callocN(sizeof(wmWidget), "widget");
	
	widget->poll = poll;
	widget->draw = draw;
	widget->handler = handler;
	widget->intersect = intersect;
	widget->render_3d_intersection = render_3d_intersection;
	widget->customdata = customdata;
	
	if (free_data)
		widget->flag |= WM_WIDGET_FREE_DATA;

	if (requires_ogl)
		widget->flag |= WM_WIDGET_REQUIRES_OGL;
	
	return widget;
	
	return NULL;
}

void WM_widgets_delete(ListBase *widgetlist, wmWidget *widget)
{
	if (widget->flag & WM_WIDGET_FREE_DATA)
		MEM_freeN(widget->customdata);
	
	BLI_freelinkN(widgetlist, widget);
}


void WM_widgets_draw(const struct bContext *C, struct ARegion *ar)
{
	if (ar->widgets->first) {
		wmWidget *widget;
		
		for (widget = ar->widgets->first; widget; widget = widget->next) {
			if ((widget->draw) &&
				(widget->poll == NULL || widget->poll(C, widget->customdata))) 
			{
				widget->draw(C, widget->customdata);			
			}
		}
	}
}

void WM_event_add_widget_handler(ARegion *ar)
{
	wmEventHandler *handler;
	
	for (handler = ar->handlers.first; handler; handler = handler->next)
		if (handler->widgets == ar->widgets)
			return;
	
	handler = MEM_callocN(sizeof(wmEventHandler), "widget handler");
	
	handler->widgets = ar->widgets;
	BLI_addhead(&ar->handlers, handler);
}


bool WM_widget_register(ListBase *widgetlist, wmWidget *widget)
{
	wmWidget *widget_iter;
	/* search list, might already be registered */	
	for (widget_iter = widgetlist->first; widget_iter; widget_iter = widget_iter->next) {
		if (widget_iter == widget)
			return false;
	}
	
	BLI_addtail(widgetlist, widget);
	return true;
}

void WM_widget_unregister(ListBase *widgetlist, wmWidget *widget)
{
	BLI_remlink(widgetlist, widget);
}

ListBase *WM_widgetmap_find(const char *idname, int spaceid, int regionid)
{
	wmWidgetMap *wmap;
	
	for (wmap = widgetmaps.first; wmap; wmap = wmap->next)
		if (wmap->spaceid == spaceid && wmap->regionid == regionid)
			if (0 == strncmp(idname, wmap->idname, KMAP_MAX_NAME))
				return &wmap->widgets;
	
	wmap = MEM_callocN(sizeof(struct wmWidgetMap), "widget list");
	BLI_strncpy(wmap->idname, idname, KMAP_MAX_NAME);
	wmap->spaceid = spaceid;
	wmap->regionid = regionid;
	BLI_addhead(&widgetmaps, wmap);
	
	return &wmap->widgets;
}

void WM_widgetmaps_free(void)
{
	wmWidgetMap *wmap;
	
	for (wmap = widgetmaps.first; wmap; wmap = wmap->next) {
		wmWidget *widget;
		
		for (widget = wmap->widgets.first; widget;) {
			wmWidget *widget_next = widget->next;
			WM_widgets_delete(&wmap->widgets, widget);
			widget = widget_next;
		}
		BLI_freelistN(&wmap->widgets);
	}
	
	BLI_freelistN(&widgetmaps);
}

wmWidget *WM_widget_find_active_3D (bContext *C, const struct wmEvent *event, float hotspot)
{
	ScrArea *sa = CTX_wm_area(C);
	View3D *v3d = sa->spacedata.first;
	ARegion *ar = CTX_wm_region(C);
	RegionView3D *rv3d = ar->regiondata;
	rctf rect, selrect;
	GLuint buffer[64];      // max 4 items per select, so large enuf
	short hits;
	const bool do_passes = GPU_select_query_check_active();
	
	/* XXX check a bit later on this... (ton) */
	extern void view3d_winmatrix_set(ARegion *ar, View3D *v3d, rctf *rect);
		
	rect.xmin = event->mval[0] - hotspot;
	rect.xmax = event->mval[0] + hotspot;
	rect.ymin = event->mval[1] - hotspot;
	rect.ymax = event->mval[1] + hotspot;
	
	selrect = rect;
	
	view3d_winmatrix_set(ar, v3d, &rect);
	mul_m4_m4m4(rv3d->persmat, rv3d->winmat, rv3d->viewmat);
	
	if (do_passes)
		GPU_select_begin(buffer, 64, &selrect, GPU_SELECT_NEAREST_FIRST_PASS, 0);
	else
		GPU_select_begin(buffer, 64, &selrect, GPU_SELECT_ALL, 0);
	
	/* do the drawing */
	
	hits = GPU_select_end();
	
	if (do_passes) {
		GPU_select_begin(buffer, 64, &selrect, GPU_SELECT_NEAREST_SECOND_PASS, hits);
		
		
		GPU_select_end();
	}
	
	view3d_winmatrix_set(ar, v3d, NULL);
	mul_m4_m4m4(rv3d->persmat, rv3d->winmat, rv3d->viewmat);
	
	return 0;
}
