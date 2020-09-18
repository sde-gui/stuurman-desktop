/*
 *      cell-placement-generator.h
 *
 *      Copyright (c) 2020 Vadim Ushakov
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with this program; if not, write to the Free Software
 *      Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *      MA 02110-1301, USA.
 */


#ifndef __CELL_PLACEMENT_GENERATOR_H__
#define __CELL_PLACEMENT_GENERATOR_H__

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
    /* bounding box to place icons */
    long left_line;
    long top_line;
    long right_line;
    long bottom_line;

    /* cell geometry */
    long cell_w;
    long cell_h;

    /* generator settings */
    int arrange_in_rows;
    int arrange_rtl; /* from right to left */
    int arrange_btt; /* from bottom to top */

    /* generated position */
    long x;
    long y;

    /* internals */
    long tier;
    long _x;
    long _y;
} CellPlacementGenerator;

static inline
void cell_placement_generator_set_bounding_box(CellPlacementGenerator * self,
    long left_line,
    long top_line,
    long right_line,
    long bottom_line)
{
    self->left_line = left_line;
    self->top_line = top_line;
    self->right_line = right_line;
    self->bottom_line = bottom_line;
}

static inline
void cell_placement_generator_set_cell_size(CellPlacementGenerator * self,
    long cell_w,
    long cell_h)
{
    self->cell_w = cell_w;
    self->cell_h = cell_h;
}

static inline
void cell_placement_generator_set_placement_rules(CellPlacementGenerator * self,
    int arrange_in_rows,
    int arrange_rtl,
    int arrange_btt)
{
    self->arrange_in_rows = arrange_in_rows;
    self->arrange_rtl = arrange_rtl;
    self->arrange_btt = arrange_btt;
}

static inline
void cell_placement_generator_reset_x(CellPlacementGenerator * self)
{
    if (self->arrange_rtl)
        self->_x = self->right_line - self->cell_w;
    else
        self->_x = self->left_line;
}

static inline
void cell_placement_generator_reset_y(CellPlacementGenerator * self)
{
    if (self->arrange_btt)
        self->_y = self->bottom_line - self->cell_h;
    else
        self->_y = self->top_line;
}

static inline
void cell_placement_generator_reset_axis1(CellPlacementGenerator * self)
{
    if (self->arrange_in_rows)
        return cell_placement_generator_reset_x(self);
    else
        return cell_placement_generator_reset_y(self);
}

static inline
void cell_placement_generator_reset_axis2(CellPlacementGenerator * self)
{
    if (self->arrange_in_rows)
        return cell_placement_generator_reset_y(self);
    else
        return cell_placement_generator_reset_x(self);
}

static inline
void cell_placement_generator_update_xy(CellPlacementGenerator * self)
{
    self->x = self->_x;
    self->y = self->_y;
    if (self->tier)
    {
        long x_div = 16;
        long y_div = 24;
        if (self->arrange_in_rows)
        {
            float t = x_div;
            x_div = y_div;
            y_div = t;
        }
        self->x += (1.0 + self->cell_w / x_div) * self->tier * (1 - 2 * self->arrange_rtl);
        self->y += (1.0 + self->cell_h / y_div) * self->tier * (1 - 2 * self->arrange_btt);
    }
}

static inline
void cell_placement_generator_reset(CellPlacementGenerator * self)
{
    cell_placement_generator_reset_x(self);
    cell_placement_generator_reset_y(self);
    self->tier = 0;
    cell_placement_generator_update_xy(self);
}

static inline
int cell_placement_generator_advance_x(CellPlacementGenerator * self)
{
    if (self->arrange_rtl)
    {
        self->_x -= self->cell_w;
        if (self->_x < self->left_line)
            return 1;
    }
    else
    {
        self->_x += self->cell_w;
        if (self->_x > self->right_line - self->cell_w)
            return 1;
    }

    return 0;
}

static inline
int cell_placement_generator_advance_y(CellPlacementGenerator * self)
{
    if (self->arrange_btt)
    {
        self->_y -= self->cell_h;
        if (self->_x < self->top_line)
            return 1;
    }
    else
    {
        self->_y += self->cell_h;
        if (self->_y > self->bottom_line - self->cell_h)
            return 1;
    }

    return 0;
}

static inline
int cell_placement_generator_advance_axis1(CellPlacementGenerator * self)
{
    if (self->arrange_in_rows)
        return cell_placement_generator_advance_x(self);
    else
        return cell_placement_generator_advance_y(self);
}

static inline
int cell_placement_generator_advance_axis2(CellPlacementGenerator * self)
{
    if (self->arrange_in_rows)
        return cell_placement_generator_advance_y(self);
    else
        return cell_placement_generator_advance_x(self);
}


static inline
void cell_placement_generator_advance(CellPlacementGenerator * self)
{
    if (cell_placement_generator_advance_axis1(self))
    {
        cell_placement_generator_reset_axis1(self);
        if (cell_placement_generator_advance_axis2(self))
        {
            cell_placement_generator_reset_axis2(self);
            self->tier++;
        }
    }

    cell_placement_generator_update_xy(self);
}

G_END_DECLS

#endif /* CELL_PLACEMENT_GENERATOR_H__ */
