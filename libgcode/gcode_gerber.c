/*
*  gcode_gerber.c
*  Source code file for G-Code generation, simulation, and visualization
*  library. This software is Copyright (C) 2006 by Justin Shumaker
*
*  This program is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation, either version 3 of the License, or
*  (at your option) any later version.
*
*  This program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "gcode_gerber.h"
#include "gcode_sketch.h"
#include "gcode_arc.h"
#include "gcode_line.h"
#include "gcode_util.h"
#include "gcode.h"


#define GCODE_GERBER_ARC_CCW	0
#define GCODE_GERBER_ARC_CW	1


static int
qsort_compare (const void *a, const void *b)
{
  gcode_vec3d_t *v0, *v1;

  v0 = (gcode_vec3d_t *) a;
  v1 = (gcode_vec3d_t *) b;

  if (fabs ((*v0)[2]-(*v1)[2]) < GCODE_PRECISION)
    return (0);

  return ((*v0)[2] < (*v1)[2] ? -1 : 1);
}


/*
* Adapted from Paul Bourke - October 1988
* Where 'u' is a unitized parametric value [0..1] given two end points and a test point.
*/
#define SOLVE_U(_p1, _p2, _p3, _u) { \
	gfloat_t _dist = (_p1[0]-_p2[0])*(_p1[0]-_p2[0]) + (_p1[1]-_p2[1])*(_p1[1]-_p2[1]); \
	_u = ((_p3[0]-_p1[0])*(_p2[0]-_p1[0]) + (_p3[1]-_p1[1])*(_p2[1]-_p1[1])) / _dist; }



static void
insert_trace_elbow (int *trace_elbow_num, gcode_vec3d_t **trace_elbow, uint8_t aperture_ind, gcode_gerber_aperture_t *aperture_array, gcode_vec2d_t pos)
{
  int i;
  uint8_t trace_elbow_match;

  trace_elbow_match = 0;
  for (i = 0; i < *trace_elbow_num; i++)
    if (sqrt (((*trace_elbow)[i][0] - pos[0])*((*trace_elbow)[i][0] - pos[0]) + ((*trace_elbow)[i][1] - pos[1])*((*trace_elbow)[i][1] - pos[1])) < GCODE_PRECISION && fabs ((*trace_elbow)[i][2] - aperture_array[aperture_ind].v[0]) <= GCODE_PRECISION)
      trace_elbow_match = 1;

  if (!trace_elbow_match)
  {
    *trace_elbow = (gcode_vec3d_t *) realloc (*trace_elbow, (*trace_elbow_num + 1) * sizeof (gcode_vec3d_t));
    (*trace_elbow)[*trace_elbow_num][0] = pos[0];
    (*trace_elbow)[*trace_elbow_num][1] = pos[1];
    (*trace_elbow)[*trace_elbow_num][2] = aperture_array[aperture_ind].v[0];
    (*trace_elbow_num)++;
  }
}


static int
gcode_gerber_pass1 (gcode_block_t *sketch_block, FILE *fh, int *trace_num, gcode_gerber_trace_t **trace_array, int *trace_elbow_num, gcode_vec3d_t **trace_elbow, int *exposure_num, gcode_gerber_exposure_t **exposure_array, gfloat_t offset)
{
  gcode_sketch_t *sketch;
  char buf[10], *file_buf = NULL;
  int i, file_buf_ind, file_buf_size, buf_ind, inum, aperture_num, aperture_cmd, arc_dir;
  uint8_t aperture_ind = 0, aperture_closed, trace_elbow_match;
  gcode_gerber_aperture_t *aperture_array;
  gcode_vec2d_t cur_pos = {0.0, 0.0}, cur_ij = {0.0, 0.0}, normal = {0.0, 0.0};
  gfloat_t x_scale, y_scale;


  aperture_num = 0;
  aperture_cmd = 2; /* default closed */
  aperture_array = NULL;
  aperture_closed = 1;
  x_scale = 1.0;
  y_scale = 1.0;
  arc_dir = GCODE_GERBER_ARC_CW;


  sketch = (gcode_sketch_t *) sketch_block->pdata;


  fseek (fh, 0, SEEK_END);
  file_buf_size = ftell (fh);
  fseek (fh, 0, SEEK_SET);
  file_buf = (char *) malloc (file_buf_size);
  fread (file_buf, 1, file_buf_size, fh);
  file_buf_ind = 0;

  while (file_buf_ind != file_buf_size)
  {
    if (file_buf[file_buf_ind] == '%')
    {
      file_buf_ind++;

      
      if (file_buf[file_buf_ind] == 'M' && file_buf[file_buf_ind+1] == 'O')
      {
        file_buf_ind += 2;

        /* Unit conversion */
        if (file_buf[file_buf_ind] == 'I' && file_buf[file_buf_ind+1] == 'N')
        {
          file_buf_ind += 2;
          if (sketch_block->gcode->units == GCODE_UNITS_MILLIMETER)
          {
            x_scale *= GCODE_INCH2MM;
            y_scale *= GCODE_INCH2MM;
          }
        }
        else if (file_buf[file_buf_ind] == 'M' && file_buf[file_buf_ind+1] == 'M')
        {
          file_buf_ind += 2;
          if (sketch_block->gcode->units == GCODE_UNITS_INCH)
          {
            x_scale *= GCODE_MM2INCH;
            y_scale *= GCODE_MM2INCH;
          }
        }
      }
      else if (file_buf[file_buf_ind] == 'F' && file_buf[file_buf_ind+1] == 'S' && file_buf[file_buf_ind+2] == 'L' && file_buf[file_buf_ind+3] == 'A')
      {
        file_buf_ind += 4;
        if (file_buf[file_buf_ind] == 'X')
        {
          file_buf_ind += 2;

          x_scale = 1.0;
          buf[0] = file_buf[file_buf_ind];
          buf[1] = 0;
          for (i = 0; i < atoi (buf); i++)
            x_scale *= 0.1;

          file_buf_ind++;
        }

        if (file_buf[file_buf_ind] == 'Y')
        {
          file_buf_ind += 2;

          y_scale = 1.0;
          buf[0] = file_buf[file_buf_ind];
          buf[1] = 0;
          for (i = 0; i < atoi (buf); i++)
            y_scale *= 0.1;

          file_buf_ind++;
        }
      }
      else if (file_buf[file_buf_ind] == 'A' && file_buf[file_buf_ind+1] == 'D' && file_buf[file_buf_ind+2] == 'D')
      {
        file_buf_ind += 3;
        buf[0] = file_buf[file_buf_ind];
        buf[1] = file_buf[file_buf_ind+1];
        buf[2] = 0;
        inum = atoi (buf);

        file_buf_ind += 2;
        if (file_buf[file_buf_ind] == 'C')
        {
          gfloat_t diameter;

          file_buf_ind++;
          if (file_buf[file_buf_ind] == ',')
            file_buf_ind++;

          buf_ind = 0;
          while (file_buf[file_buf_ind] != '*')
          {
            buf[buf_ind] = file_buf[file_buf_ind];
            buf_ind++;
            file_buf_ind++;
          }
          buf[buf_ind] = 0;
          diameter = atof (buf);

          aperture_array = (gcode_gerber_aperture_t *) realloc (aperture_array, (aperture_num + 1) * sizeof (gcode_gerber_aperture_t));
          aperture_array[aperture_num].type = GCODE_GERBER_APERTURE_TYPE_CIRCLE;
          aperture_array[aperture_num].ind = inum;
          aperture_array[aperture_num].v[0] = diameter + offset;
          aperture_num++;
        }
        else if (file_buf[file_buf_ind] == 'R')
        {
          gfloat_t x, y;
          file_buf_ind++;

          if (file_buf[file_buf_ind] == ',')
            file_buf_ind++;

          buf_ind = 0;
          while (file_buf[file_buf_ind] != 'X')
          {
            buf[buf_ind] = file_buf[file_buf_ind];
            buf_ind++;
            file_buf_ind++;
          }
          buf[buf_ind] = 0;
          x = atof (buf);

          file_buf_ind++; /* Skip 'X' */

          buf_ind = 0;
          while (file_buf[file_buf_ind] != '*')
          {
            buf[buf_ind] = file_buf[file_buf_ind];
            buf_ind++;
            file_buf_ind++;
          }
          buf[buf_ind] = 0;
          y = atof (buf);

          aperture_array = (gcode_gerber_aperture_t *) realloc (aperture_array, (aperture_num + 1) * sizeof (gcode_gerber_aperture_t));
          aperture_array[aperture_num].type = GCODE_GERBER_APERTURE_TYPE_RECTANGLE;
          aperture_array[aperture_num].ind = inum;
          aperture_array[aperture_num].v[0] = x + offset;
          aperture_array[aperture_num].v[1] = y + offset;
          aperture_num++;
        }
        else if (file_buf[file_buf_ind] == 'O' && file_buf[file_buf_ind+1] == 'C') /* Convert Octagon pads to Circles */
        {
          gfloat_t diameter;

          file_buf_ind += 2;
          while (file_buf[file_buf_ind] != ',')
            file_buf_ind++;
          file_buf_ind++;

          buf_ind = 0;
          while (file_buf[file_buf_ind] != '*')
          {
            buf[buf_ind] = file_buf[file_buf_ind];
            buf_ind++;
            file_buf_ind++;
          }
          buf[buf_ind] = 0;
          diameter = atof (buf);

          aperture_array = (gcode_gerber_aperture_t *) realloc (aperture_array, (aperture_num + 1) * sizeof (gcode_gerber_aperture_t));
          aperture_array[aperture_num].type = GCODE_GERBER_APERTURE_TYPE_CIRCLE;
          aperture_array[aperture_num].ind = inum;
          aperture_array[aperture_num].v[0] = diameter + offset;
          aperture_num++;
        }
      }

      /* Find closing '%' */
      while (file_buf[file_buf_ind] != '%')
        file_buf_ind++;
      file_buf_ind++;
    }
    else if (file_buf[file_buf_ind] == 'X' || file_buf[file_buf_ind] == 'Y' || file_buf[file_buf_ind] == 'I' || file_buf[file_buf_ind] == 'J')
    {
      gfloat_t pos[2];
      int xy_mask;
      int ij_mask;

      pos[0] = cur_pos[0];
      pos[1] = cur_pos[1];

      while (file_buf[file_buf_ind] != '*')
      {
        xy_mask = 0;
        ij_mask = 0;

        if (file_buf[file_buf_ind] == 'X')
        {
          file_buf_ind++;
          buf_ind = 0;
          while ((file_buf[file_buf_ind] >= '0' && file_buf[file_buf_ind] <= '9') || file_buf[file_buf_ind] == '-')
          {
            buf[buf_ind] = file_buf[file_buf_ind];
            buf_ind++;
            file_buf_ind++;
          }
          buf[buf_ind] = 0;

          pos[0] = x_scale * atof (buf);
          xy_mask |= 1;
        }

        if (file_buf[file_buf_ind] == 'Y')
        {
          file_buf_ind++;
          buf_ind = 0;
          while ((file_buf[file_buf_ind] >= '0' && file_buf[file_buf_ind] <= '9') || file_buf[file_buf_ind] == '-')
          {
            buf[buf_ind] = file_buf[file_buf_ind];
            buf_ind++;
            file_buf_ind++;
          }
          buf[buf_ind] = 0;

          pos[1] = y_scale * atof (buf);
          xy_mask |= 2;
        }

        if (file_buf[file_buf_ind] == 'I') /* I */
        {
          file_buf_ind++;
          buf_ind = 0;
          while ((file_buf[file_buf_ind] >= '0' && file_buf[file_buf_ind] <= '9') || file_buf[file_buf_ind] == '-')
          {
            buf[buf_ind] = file_buf[file_buf_ind];
            buf_ind++;
            file_buf_ind++;
          }
          buf[buf_ind] = 0;

          cur_ij[0] = x_scale * atof (buf);
          ij_mask |= 1;
        }

        if (file_buf[file_buf_ind] == 'J') /* J */
        {
          file_buf_ind++;
          buf_ind = 0;
          while ((file_buf[file_buf_ind] >= '0' && file_buf[file_buf_ind] <= '9') || file_buf[file_buf_ind] == '-')
          {
            buf[buf_ind] = file_buf[file_buf_ind];
            buf_ind++;
            file_buf_ind++;
          }
          buf[buf_ind] = 0;

          cur_ij[1] = y_scale * atof (buf);
          ij_mask |= 2;
        }


        if (file_buf[file_buf_ind] == 'D') /* Set aperture Number or cmd */
        {
          int d;

          file_buf_ind++; /* skip 'D' */
          buf[0] = file_buf[file_buf_ind];
          buf[1] = file_buf[file_buf_ind+1];
          buf[2] = 0;
          file_buf_ind += 2;


          d = atoi (buf);

          if (d > 3)
          {
            for (i = 0; i < aperture_num; i++)
              if (aperture_array[i].ind == d)
                aperture_ind = i;
          }
          else
          {
            aperture_cmd = d;
          }
        }


        if (ij_mask)
        {
          gcode_arc_t *arc;
          gcode_block_t *arc_block;
          gcode_vec2d_t center;
          gfloat_t radius, start_angle, end_angle, sweep;

          /* Calculate arc radius */
          GCODE_MATH_VEC2D_MAG (radius, cur_ij);

          /*
          * Use the tangent of the previous trace to determine start angle of arc.
          * Use the tangent of the previous trace to calculate the normal to space the arcs apart.
          * Starting point of arc is simply current position.
          * NOTES!!!
          * - An elbow may need to be inserted into the list after an arc.
          * - Store a previous normal.
          * - Make sure that cur_pos gets updated when a G02/G03 occures, not just after a line.
          * - Look into whether or not these arcs should exist in the trace list (duplicity etc).
          */


          /* Calculate start angle and sweep angle based on current position and destination. */
          GCODE_MATH_VEC2D_ADD (center, cur_ij, cur_pos);

          gcode_math_xy_to_angle (center, radius, cur_pos[0], cur_pos[1], &start_angle);
          gcode_math_xy_to_angle (center, radius, pos[0], pos[1], &end_angle);

          if (end_angle < start_angle)
            end_angle += 360.0;

/* printf ("arc pos: %f %f, start_angle: %f, end_angle: %f\n", cur_pos[0], cur_pos[1], start_angle, end_angle); */

          if (arc_dir == GCODE_GERBER_ARC_CW)
          {
            sweep = start_angle - end_angle;

/* printf ("  make a CW arc: %f %f, radius: %f  normal: %f %f, sweep: %f\n", cur_ij[0], cur_ij[1], radius, normal[0], normal[1], sweep); */
          }
          else if (arc_dir == GCODE_GERBER_ARC_CCW)
          {
            if (end_angle < start_angle)
              end_angle += 360.0;

            sweep = end_angle - start_angle;

/* printf ("  make a CCW arc: %f %f, radius: %f  normal: %f %f, sweep: %f\n", cur_ij[0], cur_ij[1], radius, normal[0], normal[1], sweep); */
          }

          /* Arc 1 */
          gcode_arc_init (sketch_block->gcode, &arc_block, sketch_block);
          arc_block->offset = &sketch->offset;
          arc_block->prev = NULL;
          arc_block->next = NULL;
          gcode_list_insert (&sketch->list, arc_block);

          arc = (gcode_arc_t *) arc_block->pdata;
          arc->pos[0] = cur_pos[0] + 0.5*normal[0]*aperture_array[aperture_ind].v[0];
          arc->pos[1] = cur_pos[1] + 0.5*normal[1]*aperture_array[aperture_ind].v[0];
          arc->start_angle = start_angle;
          arc->sweep = sweep;
          arc->radius = radius;

          /* Arc 2 */
          gcode_arc_init (sketch_block->gcode, &arc_block, sketch_block);
          arc_block->offset = &sketch->offset;
          arc_block->prev = NULL;
          arc_block->next = NULL;
          gcode_list_insert (&sketch->list, arc_block);

          arc = (gcode_arc_t *) arc_block->pdata;
          arc->pos[0] = cur_pos[0] - 0.5*normal[0]*aperture_array[aperture_ind].v[0];
          arc->pos[1] = cur_pos[1] - 0.5*normal[1]*aperture_array[aperture_ind].v[0];
          arc->start_angle = start_angle;
          arc->sweep = sweep;
          arc->radius = radius;

          insert_trace_elbow (trace_elbow_num, trace_elbow, aperture_ind, aperture_array, pos);
        }
        else if (xy_mask) /* And X or Y has occured - Uses previous aperture_cmd if a new one isn't present. */
        {
          if (aperture_cmd == 1) /* Open Exposure - Trace (line) */
          {
            gcode_block_t *line_block;
            gcode_line_t *line;
            gfloat_t mag, dist0, dist1;
            int duplicate_trace;
          
/* printf ("trace\n"); */
            /* Store the Trace - Check for Duplicates before storing */
            duplicate_trace = 0;
            for (i = 0; i < *trace_num; i++)
            {
              dist0 = sqrt (((*trace_array)[i].p0[0]-cur_pos[0])*((*trace_array)[i].p0[0]-cur_pos[0]) + ((*trace_array)[i].p0[1]-cur_pos[1])*((*trace_array)[i].p0[1]-cur_pos[1]));
              dist1 = sqrt (((*trace_array)[i].p1[0]-pos[0])*((*trace_array)[i].p1[0]-pos[0]) + ((*trace_array)[i].p1[1]-pos[1])*((*trace_array)[i].p1[1]-pos[1]));
              if (dist0 < GCODE_PRECISION && dist1 < GCODE_PRECISION)
                duplicate_trace = 1;

              dist0 = sqrt (((*trace_array)[i].p0[0]-pos[0])*((*trace_array)[i].p0[0]-pos[0]) + ((*trace_array)[i].p0[1]-pos[1])*((*trace_array)[i].p0[1]-pos[1]));
              dist1 = sqrt (((*trace_array)[i].p1[0]-cur_pos[0])*((*trace_array)[i].p1[0]-cur_pos[0]) + ((*trace_array)[i].p1[1]-cur_pos[1])*((*trace_array)[i].p1[1]-cur_pos[1]));
              if (dist0 < GCODE_PRECISION && dist1 < GCODE_PRECISION)
                duplicate_trace = 1;
            }

            normal[0] = cur_pos[1] - pos[1];
            normal[1] = pos[0] - cur_pos[0];
            mag = 1.0 / sqrt (normal[0]*normal[0] + normal[1]*normal[1]);
            normal[0] *= mag;
            normal[1] *= mag;

            if (!duplicate_trace)
            {
              *trace_array = (gcode_gerber_trace_t *) realloc (*trace_array, (*trace_num + 1) * sizeof (gcode_gerber_trace_t));
              (*trace_array)[*trace_num].p0[0] = cur_pos[0];
              (*trace_array)[*trace_num].p0[1] = cur_pos[1];
              (*trace_array)[*trace_num].p1[0] = pos[0];
              (*trace_array)[*trace_num].p1[1] = pos[1];
              (*trace_array)[*trace_num].radius = 0.5 * aperture_array[aperture_ind].v[0];
              (*trace_num)++;

              /* Line 1 */
              gcode_line_init (sketch_block->gcode, &line_block, sketch_block);
              line_block->offset = &sketch->offset;
              line_block->prev = NULL;
              line_block->next = NULL;
              gcode_list_insert (&sketch->list, line_block);

              line = (gcode_line_t *) line_block->pdata;
              line->p0[0] = cur_pos[0] + 0.5*normal[0]*aperture_array[aperture_ind].v[0];
              line->p0[1] = cur_pos[1] + 0.5*normal[1]*aperture_array[aperture_ind].v[0];
              line->p1[0] = pos[0] + 0.5*normal[0]*aperture_array[aperture_ind].v[0];
              line->p1[1] = pos[1] + 0.5*normal[1]*aperture_array[aperture_ind].v[0];

              /* Line 2 */
              gcode_line_init (sketch_block->gcode, &line_block, sketch_block);
              line_block->offset = &sketch->offset;
              line_block->prev = NULL;
              line_block->next = NULL;
              gcode_list_insert (&sketch->list, line_block);

              line = (gcode_line_t *) line_block->pdata;
              line->p0[0] = cur_pos[0] - 0.5*normal[0]*aperture_array[aperture_ind].v[0];
              line->p0[1] = cur_pos[1] - 0.5*normal[1]*aperture_array[aperture_ind].v[0];
              line->p1[0] = pos[0] - 0.5*normal[0]*aperture_array[aperture_ind].v[0];
              line->p1[1] = pos[1] - 0.5*normal[1]*aperture_array[aperture_ind].v[0];


              /* If the aperture was previously closed insert an elbow - check both position and diameter for duplicity */
              if (aperture_closed)
              {
                insert_trace_elbow (trace_elbow_num, trace_elbow, aperture_ind, aperture_array, cur_pos);

                aperture_closed = 0;
              }

              /* Insert an elbow at the end of this trace segment - check both position and diameter for duplicity */
/* XXX - Elbows need work for arcs */
              insert_trace_elbow (trace_elbow_num, trace_elbow, aperture_ind, aperture_array, pos);
            }
          }
          else if (aperture_cmd == 2) /* Aperture Closed */
          {
            aperture_closed = 1;
          }
          else if (aperture_cmd == 3) /* Flash exposure */
          {
            if (aperture_array[aperture_ind].type == GCODE_GERBER_APERTURE_TYPE_CIRCLE)
            {
              gcode_block_t *arc_block;
              gcode_arc_t *arc;

              /* arc 1 */
              gcode_arc_init (sketch_block->gcode, &arc_block, sketch_block);
              arc_block->offset = &sketch->offset;
              arc_block->prev = NULL;
              arc_block->next = NULL;
              gcode_list_insert (&sketch->list, arc_block);

              arc = (gcode_arc_t *) arc_block->pdata;
              arc->pos[0] = pos[0] - 0.5 * aperture_array[aperture_ind].v[0];
              arc->pos[1] = pos[1];
              arc->start_angle = 180.0;
              arc->sweep = -360.0;
              arc->radius = 0.5 * aperture_array[aperture_ind].v[0];
/* printf ("flash[%d]: radius %f\n", aperture_ind, aperture_array[aperture_ind].v[0]); */

              *exposure_array = (gcode_gerber_exposure_t *) realloc (*exposure_array, (*exposure_num + 1) * sizeof (gcode_gerber_exposure_t));
              (*exposure_array)[*exposure_num].type = GCODE_GERBER_APERTURE_TYPE_CIRCLE;
              (*exposure_array)[*exposure_num].pos[0] = pos[0];
              (*exposure_array)[*exposure_num].pos[1] = pos[1];
              (*exposure_array)[*exposure_num].v[0] = aperture_array[aperture_ind].v[0];
              (*exposure_num)++;
            }
            else if (aperture_array[aperture_ind].type == GCODE_GERBER_APERTURE_TYPE_RECTANGLE)
            {
              gcode_block_t *line_block;
              gcode_line_t *line;

              /* Line 1 */
              gcode_line_init (sketch_block->gcode, &line_block, sketch_block);
              line_block->offset = &sketch->offset;
              line_block->prev = NULL;
              line_block->next = NULL;
              gcode_list_insert (&sketch->list, line_block);

              line = (gcode_line_t *) line_block->pdata;
              line->p0[0] = pos[0] - 0.5*aperture_array[aperture_ind].v[0];
              line->p0[1] = pos[1] + 0.5*aperture_array[aperture_ind].v[1];
              line->p1[0] = pos[0] + 0.5*aperture_array[aperture_ind].v[0];
              line->p1[1] = pos[1] + 0.5*aperture_array[aperture_ind].v[1];

              /* Line 2 */
              gcode_line_init (sketch_block->gcode, &line_block, sketch_block);
              line_block->offset = &sketch->offset;
              line_block->prev = NULL;
              line_block->next = NULL;
              gcode_list_insert (&sketch->list, line_block);

              line = (gcode_line_t *) line_block->pdata;
              line->p0[0] = pos[0] + 0.5*aperture_array[aperture_ind].v[0];
              line->p0[1] = pos[1] + 0.5*aperture_array[aperture_ind].v[1];
              line->p1[0] = pos[0] + 0.5*aperture_array[aperture_ind].v[0];
              line->p1[1] = pos[1] - 0.5*aperture_array[aperture_ind].v[1];

              /* Line 3 */
              gcode_line_init (sketch_block->gcode, &line_block, sketch_block);
              line_block->offset = &sketch->offset;
              line_block->prev = NULL;
              line_block->next = NULL;
              gcode_list_insert (&sketch->list, line_block);

              line = (gcode_line_t *) line_block->pdata;
              line->p0[0] = pos[0] + 0.5*aperture_array[aperture_ind].v[0];
              line->p0[1] = pos[1] - 0.5*aperture_array[aperture_ind].v[1];
              line->p1[0] = pos[0] - 0.5*aperture_array[aperture_ind].v[0];
              line->p1[1] = pos[1] - 0.5*aperture_array[aperture_ind].v[1];

              /* Line 4 */
              gcode_line_init (sketch_block->gcode, &line_block, sketch_block);
              line_block->offset = &sketch->offset;
              line_block->prev = NULL;
              line_block->next = NULL;
              gcode_list_insert (&sketch->list, line_block);
              line = (gcode_line_t *) line_block->pdata;
              line->p0[0] = pos[0] - 0.5*aperture_array[aperture_ind].v[0];
              line->p0[1] = pos[1] - 0.5*aperture_array[aperture_ind].v[1];
              line->p1[0] = pos[0] - 0.5*aperture_array[aperture_ind].v[0];
              line->p1[1] = pos[1] + 0.5*aperture_array[aperture_ind].v[1];

              *exposure_array = (gcode_gerber_exposure_t *) realloc (*exposure_array, (*exposure_num + 1) * sizeof (gcode_gerber_exposure_t));
              (*exposure_array)[*exposure_num].type = GCODE_GERBER_APERTURE_TYPE_RECTANGLE;
              (*exposure_array)[*exposure_num].pos[0] = pos[0];
              (*exposure_array)[*exposure_num].pos[1] = pos[1];
              (*exposure_array)[*exposure_num].v[0] = aperture_array[aperture_ind].v[0];
              (*exposure_array)[*exposure_num].v[1] = aperture_array[aperture_ind].v[1];
              (*exposure_num)++;
            }
          }
        }

        /* Update current position */
        if (xy_mask&1)
          cur_pos[0] = pos[0];
        if (xy_mask&2)
          cur_pos[1] = pos[1];
      }
    }
    else if (file_buf[file_buf_ind] == 'G')
    {
      file_buf_ind++;


      if (file_buf[file_buf_ind] == '0' && file_buf[file_buf_ind+1] == '1')
      {
        /* Linear interpolation - Line */
        file_buf_ind += 2;
        /* Using current position, generate a line. */
      }
      else if (file_buf[file_buf_ind] == '0' && file_buf[file_buf_ind+1] == '2')
      {
        /* Clockwise circular interpolation */
        file_buf_ind += 2;
        /* Using current position, generate a CW arc. */
        arc_dir = GCODE_GERBER_ARC_CW;
      }
      else if (file_buf[file_buf_ind] == '0' && file_buf[file_buf_ind+1] == '3')
      {
        /* Counter Clockwise circular interpolation */
        file_buf_ind += 2;
        /* Using current position, generate a CCW arc. */
        arc_dir = GCODE_GERBER_ARC_CCW;
      }
      else if (file_buf[file_buf_ind] == '0' && file_buf[file_buf_ind+1] == '4')
      {
        /* Ignore data block */
        file_buf_ind += 2;
        while (file_buf[file_buf_ind] != '\n')
          file_buf_ind++;
      }
      else if (file_buf[file_buf_ind] == '5' && file_buf[file_buf_ind+1] == '4')
      {
        /* Tool Prepare */
        file_buf_ind += 2;

        file_buf_ind++; /* skip 'D' */
        buf[0] = file_buf[file_buf_ind];
        buf[1] = file_buf[file_buf_ind+1];
        buf[2] = 0;
        file_buf_ind += 2;

        for (i = 0; i < aperture_num; i++)
          if (aperture_array[i].ind == atoi (buf))
            aperture_ind = i;
      }
      else if (file_buf[file_buf_ind] == '7' && file_buf[file_buf_ind+1] == '0')
      {
        /* Specify Inches - do nothing for now */
        file_buf_ind += 2;
      }
      else if (file_buf[file_buf_ind] == '7' && file_buf[file_buf_ind+1] == '1')
      {
        /* Specify millimeters - do nothing for now */
        file_buf_ind += 2;
      }
      else if (file_buf[file_buf_ind] == '7' && file_buf[file_buf_ind+1] == '5')
      {
        /* Enable 360 degree circular interpolation (multiquadrant) */
        file_buf_ind += 2;
      }
    }
    else
    {
      file_buf_ind++;
    }
  }

  return (0);
}


static void
gcode_gerber_pass2 (gcode_block_t *sketch_block, int trace_elbow_num, gcode_vec3d_t *trace_elbow)
{
  /*
  * PASS 2
  * Insert Trace Elbows
  */
  gcode_block_t *arc_block;
  gcode_sketch_t *sketch;
  gcode_arc_t *arc;
  int i;

  sketch = (gcode_sketch_t *) sketch_block->pdata;

  for (i = 0; i < trace_elbow_num; i++)
  {
    /* Circle Transition */
    gcode_arc_init (sketch_block->gcode, &arc_block, sketch_block);
    arc_block->offset = &sketch->offset;
    arc_block->prev = NULL;
    arc_block->next = NULL;
    gcode_list_insert (&sketch->list, arc_block);

    arc = (gcode_arc_t *) arc_block->pdata;
    arc->radius = 0.5 * trace_elbow[i][2];
    arc->pos[0] = trace_elbow[i][0] - arc->radius;
    arc->pos[1] = trace_elbow[i][1];
    arc->start_angle = 180;
    arc->sweep = -360.0;
  }
}


static void
gcode_gerber_pass3 (gcode_block_t *sketch_block)
{
  /*
  * PASS 3 - Create Intersections.
  * Loop through each line and arc and segment for each intersection.
  */
  gcode_block_t *index1_block, *index2_block, *intersection_list = NULL;
  gcode_sketch_t *sketch;
  gcode_vec3d_t full_ip_sorted_array[256];
  gcode_vec2d_t full_ip_array[256], ip_array[2];
  int i, full_ip_num, ip_num;

  sketch = (gcode_sketch_t *) sketch_block->pdata;

  index1_block = sketch->list;
  while (index1_block)
  {
    full_ip_num = 0;

    index2_block = sketch->list;
    while (index2_block)
    {
      if (index1_block != index2_block) /* Don't perform intersection against self. */
      {
        if (!gcode_util_intersect (index1_block, index2_block, ip_array, &ip_num))
        {
          for (i = 0; i < ip_num; i++)
          {
            full_ip_array[full_ip_num][0] = ip_array[i][0];
            full_ip_array[full_ip_num][1] = ip_array[i][1];
            full_ip_num++;
          }
        }
      }
      index2_block = index2_block->next;
    }

    if (index1_block->type == GCODE_TYPE_LINE)
    {
      gcode_block_t *line_block;
      gcode_line_t *line, *new_line;

      line = (gcode_line_t *) index1_block->pdata;

      /*
      * Generate line segments in order from line->p0 to p1 by sorting
      * the full_ip_num based on distance from line->p0.
      */
      if (full_ip_num)
      {
        /* There were intersections. */
        for (i = 0; i < full_ip_num; i++)
        {
          full_ip_sorted_array[i][0] = full_ip_array[i][0];
          full_ip_sorted_array[i][1] = full_ip_array[i][1];
          full_ip_sorted_array[i][2] = sqrt ((line->p0[0]-full_ip_array[i][0])*(line->p0[0]-full_ip_array[i][0]) + (line->p0[1]-full_ip_array[i][1])*(line->p0[1]-full_ip_array[i][1]));
        }
        qsort (full_ip_sorted_array, full_ip_num, sizeof (gcode_vec3d_t), qsort_compare);

        /* First point to first segment */
        if (full_ip_sorted_array[0][2] > GCODE_PRECISION)
        {
          gcode_line_init (sketch_block->gcode, &line_block, sketch_block);
          line_block->offset = &sketch->offset;
          line_block->prev = NULL;
          line_block->next = NULL;
          gcode_list_insert (&intersection_list, line_block);

          new_line = (gcode_line_t *) line_block->pdata;
          new_line->p0[0] = line->p0[0];
          new_line->p0[1] = line->p0[1];
          new_line->p1[0] = full_ip_sorted_array[0][0];
          new_line->p1[1] = full_ip_sorted_array[0][1];
        }

        /* Generate a line segment from full_ip_sorted_array[n] to full_ip_sorted_array[n+1] */
        for (i = 0; i < full_ip_num-1; i++)
        {
          if (full_ip_sorted_array[i+1][2] - full_ip_sorted_array[i][2] > GCODE_PRECISION)
          {
            gcode_line_init (sketch_block->gcode, &line_block, sketch_block);
            line_block->offset = &sketch->offset;
            line_block->prev = NULL;
            line_block->next = NULL;
            gcode_list_insert (&intersection_list, line_block);

            new_line = (gcode_line_t *) line_block->pdata;
            new_line->p0[0] = full_ip_sorted_array[i][0];
            new_line->p0[1] = full_ip_sorted_array[i][1];
            new_line->p1[0] = full_ip_sorted_array[i+1][0];
            new_line->p1[1] = full_ip_sorted_array[i+1][1];
          }
        }

        /* Last segment to last point */
        if (sqrt ((line->p1[0]-full_ip_sorted_array[full_ip_num-1][0])*(line->p1[0]-full_ip_sorted_array[full_ip_num-1][0])+(line->p1[1]-full_ip_sorted_array[full_ip_num-1][1])*(line->p1[1]-full_ip_sorted_array[full_ip_num-1][1])) > GCODE_PRECISION)
        {
          gcode_line_init (sketch_block->gcode, &line_block, sketch_block);
          line_block->offset = &sketch->offset;
          line_block->prev = NULL;
          line_block->next = NULL;
          gcode_list_insert (&intersection_list, line_block);

          new_line = (gcode_line_t *) line_block->pdata;
          new_line->p0[0] = full_ip_sorted_array[full_ip_num-1][0];
          new_line->p0[1] = full_ip_sorted_array[full_ip_num-1][1];
          new_line->p1[0] = line->p1[0];
          new_line->p1[1] = line->p1[1];
        }

      }
      else
      {
        /* Just copy the line, do nothing, no intersections. */
        gcode_line_init (sketch_block->gcode, &line_block, sketch_block);
        line_block->offset = &sketch->offset;
        line_block->prev = NULL;
        line_block->next = NULL;
        gcode_list_insert (&intersection_list, line_block);

        new_line = (gcode_line_t *) line_block->pdata;
        new_line->p0[0] = line->p0[0];
        new_line->p0[1] = line->p0[1];
        new_line->p1[0] = line->p1[0];
        new_line->p1[1] = line->p1[1];
      }
    }
    else if (index1_block->type == GCODE_TYPE_ARC)
    {
      /* XXX - ARCS exist either as traces or elbows */
      gcode_block_t *arc_block;
      gcode_arc_t *arc, *new_arc;
      gcode_vec2d_t center;
      gfloat_t angle;

      arc = (gcode_arc_t *) index1_block->pdata;

      if (!full_ip_num)
      {
        /* Just copy the arc, do nothing, no intersections. */
        gcode_arc_init (sketch_block->gcode, &arc_block, sketch_block);
        arc_block->offset = &sketch->offset;
        arc_block->prev = NULL;
        arc_block->next = NULL;
        gcode_list_insert (&intersection_list, arc_block);

        new_arc = (gcode_arc_t *) arc_block->pdata;
        new_arc->pos[0] = arc->pos[0];
        new_arc->pos[1] = arc->pos[1];
        new_arc->start_angle = arc->start_angle;
        new_arc->sweep = arc->sweep;
        new_arc->radius = arc->radius;
      }
      else
      {
        gcode_vec2d_t origin, p0, span, test_span;
        gfloat_t radius_offset, mid;

        /* There were intersections. */
        gcode_arc_with_offset (index1_block, origin, center, p0, &radius_offset, &angle);

        for (i = 0; i < full_ip_num; i++)
        {
          gcode_math_xy_to_angle (center, arc->radius, full_ip_array[i][0], full_ip_array[i][1], &angle);

          full_ip_sorted_array[i][0] = full_ip_array[i][0];
          full_ip_sorted_array[i][1] = full_ip_array[i][1];
          full_ip_sorted_array[i][2] = angle;
        }
        qsort (full_ip_sorted_array, full_ip_num, sizeof (gcode_vec3d_t), qsort_compare);

        /* 
        * You have the original start angle and sweep, therefore you can prevent arcs from being formed outside that region.
        * original arc is: "arc"
        */
        for (i = 0; i < full_ip_num-1; i++)
        {
          if (arc->radius > GCODE_PRECISION && full_ip_sorted_array[i+1][2] - full_ip_sorted_array[i][2] > GCODE_ANGULAR_PRECISION) /* Make sure the arc is big enough */
          {
            if (arc->sweep > 0.0)
            {
              span[0] = arc->start_angle;
              span[1] = arc->start_angle + arc->sweep;
            }
            else
            {
              span[0] = arc->start_angle + arc->sweep;
              span[1] = arc->start_angle;
            }


            if (full_ip_sorted_array[i+1][2] - full_ip_sorted_array[i][2] > 0.0)
            {
              test_span[0] = full_ip_sorted_array[i][2];
              test_span[1] = full_ip_sorted_array[i][2] + (full_ip_sorted_array[i+1][2] - full_ip_sorted_array[i][2]);
            }
            else
            {
              test_span[0] = full_ip_sorted_array[i][2] + (full_ip_sorted_array[i+1][2] - full_ip_sorted_array[i][2]);
              test_span[1] = full_ip_sorted_array[i][2];
            }

            mid = (test_span[0] + test_span[1]) * 0.5;


/*            if (test_span[0] >= span[0]-GCODE_ANGULAR_PRECISION && test_span[1] <= span[1] + GCODE_ANGULAR_PRECISION) */

/* printf ("test_span: %f..%f and span: %f..%f\n", test_span[0], test_span[1], span[0], span[1]); */
          if (((test_span[0] >= span[0]-GCODE_ANGULAR_PRECISION-360.0 && test_span[0] <= span[1]+GCODE_ANGULAR_PRECISION-360.0) || (test_span[0] >= span[0]-GCODE_ANGULAR_PRECISION && test_span[0] <= span[1]+GCODE_ANGULAR_PRECISION) || (test_span[0] >= span[0]-GCODE_ANGULAR_PRECISION+360.0 && test_span[0] <= span[1]+GCODE_ANGULAR_PRECISION+360.0)) &&
              ((test_span[1] >= span[0]-GCODE_ANGULAR_PRECISION-360.0 && test_span[1] <= span[1]+GCODE_ANGULAR_PRECISION-360.0) || (test_span[1] >= span[0]-GCODE_ANGULAR_PRECISION && test_span[1] <= span[1]+GCODE_ANGULAR_PRECISION) || (test_span[1] >= span[0]-GCODE_ANGULAR_PRECISION+360.0 && test_span[1] <= span[1]+GCODE_ANGULAR_PRECISION+360.0)) &&
              ((mid >= span[0]-GCODE_ANGULAR_PRECISION-360.0 && mid <= span[1]+GCODE_ANGULAR_PRECISION-360.0) || (mid >= span[0]-GCODE_ANGULAR_PRECISION && mid <= span[1]+GCODE_ANGULAR_PRECISION) || (mid >= span[0]-GCODE_ANGULAR_PRECISION+360.0 && mid <= span[1]+GCODE_ANGULAR_PRECISION+360.0)))
 /* Make sure that new arc falls within the existing arcs region. */
            {
/* printf ("center: %f,%f .. start_angle: %f\n", center[0], center[1], full_ip_sorted_array[i][2]); */
              gcode_arc_init (sketch_block->gcode, &arc_block, sketch_block);
              arc_block->offset = &sketch->offset;
              arc_block->prev = NULL;
              arc_block->next = NULL;
              gcode_list_insert (&intersection_list, arc_block);

              new_arc = (gcode_arc_t *) arc_block->pdata;
              new_arc->pos[0] = full_ip_sorted_array[i][0];
              new_arc->pos[1] = full_ip_sorted_array[i][1];
/* printf ("pos: %f,%f\n", new_arc->pos[0], new_arc->pos[1]); */
              new_arc->start_angle = full_ip_sorted_array[i][2];
              new_arc->sweep = full_ip_sorted_array[i+1][2] - full_ip_sorted_array[i][2];
/* printf ("sweep1: %f\n", new_arc->sweep); */
              new_arc->radius = arc->radius;
            }
            else
            {
/*              printf ("test_span: %f..%f, span: %f..%f\n", test_span[0], test_span[1], span[0], span[1]); */
            }
          }
        }


        /* Link the Last segment with the first one (if arc is a complete circle) */
/* printf ("%f - %f\n", full_ip_sorted_array[full_ip_num-1][2], full_ip_sorted_array[0][2]); */
/*        if (GCODE_DEG2RAD * (360.0 - (full_ip_sorted_array[full_ip_num-1][2] - full_ip_sorted_array[0][2])) * arc->radius > GCODE_ANGULAR_PRECISION) */
/*        { */
          gcode_arc_init (sketch_block->gcode, &arc_block, sketch_block);
          arc_block->offset = &sketch->offset;
          arc_block->prev = NULL;
          arc_block->next = NULL;
          gcode_list_insert (&intersection_list, arc_block);

          new_arc = (gcode_arc_t *) arc_block->pdata;
          new_arc->pos[0] = full_ip_sorted_array[full_ip_num-1][0];
          new_arc->pos[1] = full_ip_sorted_array[full_ip_num-1][1];
          new_arc->start_angle = full_ip_sorted_array[full_ip_num-1][2];
          new_arc->sweep = 360.0 - (full_ip_sorted_array[full_ip_num-1][2] - full_ip_sorted_array[0][2]);
/* printf ("sweep2: %f\n", new_arc->sweep);*/
          new_arc->radius = arc->radius;
/*        } */
      }
    }

    index1_block = index1_block->next;
  }

  gcode_list_remove (sketch->list);
  sketch->list = intersection_list;

  /*
  * Set parent and parent_list to be the correct pointers for all blocks.
  * This will allow removal of blocks to operate on valid pointers.
  */
  index1_block = sketch->list;
  while (index1_block)
  {
    index1_block->parent = sketch_block;
    index1_block->parent_list = &sketch->list;
    index1_block = index1_block->next;
  }
}


static void
gcode_gerber_pass4 (gcode_block_t *sketch_block, int trace_num, gcode_gerber_trace_t *trace_array, int exposure_num, gcode_gerber_exposure_t *exposure_array)
{
  /*
  * PASS 4 - Eliminate internal intersections.
  */
  gcode_block_t *line_block, *index1_block, *index2_block;
  gcode_sketch_t *sketch;
  gcode_line_t *line;
  gcode_arc_t *arc;
  gcode_vec2d_t ip_array[2], pos[2], dpos, center;
  int i, ip_num, remove;
  gfloat_t dist, u;

  sketch = (gcode_sketch_t *) sketch_block->pdata;

  gcode_line_init (sketch_block->gcode, &line_block, sketch_block);
  line_block->offset = &sketch->offset;
  line_block->prev = NULL;
  line_block->next = NULL;
  line = (gcode_line_t *) line_block->pdata;

  /*
  * Trace Intersection Check
  * Intersect the original trace with every block.
  * The key here is that the original trace runs down the center.
  */
  for (i = 0; i < trace_num; i++)
  {
    line->p0[0] = trace_array[i].p0[0];
    line->p0[1] = trace_array[i].p0[1];
    line->p1[0] = trace_array[i].p1[0];
    line->p1[1] = trace_array[i].p1[1];

    index1_block = sketch->list;
    while (index1_block)
    {
      if (!gcode_util_intersect (line_block, index1_block, ip_array, &ip_num))
      {
        /* Remove block */
        index2_block = index1_block;
        index1_block = index1_block->next;
        gcode_list_remove (index2_block);
      }
      else
      {
        index1_block = index1_block->next;
      }
    }
  }


  /* Inside Exposure (PAD) Check */
  for (i = 0; i < exposure_num; i++)
  {
    index1_block = sketch->list;
    while (index1_block)
    {
      index1_block->ends (index1_block, pos[0], pos[1], GCODE_GET);
      remove = 0;
      center[0] = 0.5 * (pos[0][0] + pos[1][0]);
      center[1] = 0.5 * (pos[0][1] + pos[1][1]);

      if (exposure_array[i].type == GCODE_GERBER_APERTURE_TYPE_CIRCLE)
      {
        dist = sqrt ((pos[0][0]-exposure_array[i].pos[0])*(pos[0][0]-exposure_array[i].pos[0]) + (pos[0][1]-exposure_array[i].pos[1])*(pos[0][1]-exposure_array[i].pos[1]));
        if (dist < 0.5*exposure_array[i].v[0]-GCODE_PRECISION)
          remove = 1; 

        dist = sqrt ((pos[1][0]-exposure_array[i].pos[0])*(pos[1][0]-exposure_array[i].pos[0]) + (pos[1][1]-exposure_array[i].pos[1])*(pos[1][1]-exposure_array[i].pos[1]));
        if (dist < 0.5*exposure_array[i].v[0]-GCODE_PRECISION)
          remove = 1; 

        if (index1_block->type == GCODE_TYPE_LINE)
          dist = sqrt ((center[0]-exposure_array[i].pos[0])*(center[0]-exposure_array[i].pos[0]) + (center[1]-exposure_array[i].pos[1])*(center[1]-exposure_array[i].pos[1]));
          if (dist < 0.5*exposure_array[i].v[0]-GCODE_PRECISION)
            remove = 1; 

        if (index1_block->type == GCODE_TYPE_ARC)
        {
          gcode_vec2d_t origin, arc_center, p0;
          gfloat_t arc_radius_offset, start_angle;

          arc = (gcode_arc_t *) index1_block->pdata;

          gcode_arc_with_offset (index1_block, origin, arc_center, p0, &arc_radius_offset, &start_angle);

          p0[0] = arc_center[0] + arc->radius * cos (GCODE_DEG2RAD * (start_angle + arc->sweep * 0.5));
          p0[1] = arc_center[1] + arc->radius * sin (GCODE_DEG2RAD * (start_angle + arc->sweep * 0.5));

          dist = sqrt ((p0[0]-exposure_array[i].pos[0])*(p0[0]-exposure_array[i].pos[0]) + (p0[1]-exposure_array[i].pos[1])*(p0[1]-exposure_array[i].pos[1]));
          if (dist < 0.5*exposure_array[i].v[0]-GCODE_PRECISION)
            remove = 1;
        }
      }
      else if (exposure_array[i].type == GCODE_GERBER_APERTURE_TYPE_RECTANGLE)
      {
        if (pos[0][0] > exposure_array[i].pos[0]-0.5*exposure_array[i].v[0]+GCODE_PRECISION && pos[0][0] < exposure_array[i].pos[0]+0.5*exposure_array[i].v[0]-GCODE_PRECISION &&
            pos[0][1] > exposure_array[i].pos[1]-0.5*exposure_array[i].v[1]+GCODE_PRECISION && pos[0][1] < exposure_array[i].pos[1]+0.5*exposure_array[i].v[1]-GCODE_PRECISION)
           remove = 1;

        if (pos[1][0] > exposure_array[i].pos[0]-0.5*exposure_array[i].v[0]+GCODE_PRECISION && pos[1][0] < exposure_array[i].pos[0]+0.5*exposure_array[i].v[0]-GCODE_PRECISION &&
            pos[1][1] > exposure_array[i].pos[1]-0.5*exposure_array[i].v[1]+GCODE_PRECISION && pos[1][1] < exposure_array[i].pos[1]+0.5*exposure_array[i].v[1]-GCODE_PRECISION)
           remove = 1;

        if (index1_block->type == GCODE_TYPE_LINE)
          if (center[0] > exposure_array[i].pos[0]-0.5*exposure_array[i].v[0]+GCODE_PRECISION && center[0] < exposure_array[i].pos[0]+0.5*exposure_array[i].v[0]-GCODE_PRECISION &&
              center[1] > exposure_array[i].pos[1]-0.5*exposure_array[i].v[1]+GCODE_PRECISION && center[1] < exposure_array[i].pos[1]+0.5*exposure_array[i].v[1]-GCODE_PRECISION)
             remove = 1;

        if (index1_block->type == GCODE_TYPE_ARC)
        {
          gcode_vec2d_t origin, arc_center, p0;
          gfloat_t arc_radius_offset, start_angle;

          arc = (gcode_arc_t *) index1_block->pdata;

          gcode_arc_with_offset (index1_block, origin, arc_center, p0, &arc_radius_offset, &start_angle);

          p0[0] = arc_center[0] + arc->radius * cos (GCODE_DEG2RAD * (start_angle + arc->sweep * 0.5));
          p0[1] = arc_center[1] + arc->radius * sin (GCODE_DEG2RAD * (start_angle + arc->sweep * 0.5));

          if (p0[0] > exposure_array[i].pos[0]-0.5*exposure_array[i].v[0]+GCODE_PRECISION && p0[0] < exposure_array[i].pos[0]+0.5*exposure_array[i].v[0]-GCODE_PRECISION &&
              p0[1] > exposure_array[i].pos[1]-0.5*exposure_array[i].v[1]+GCODE_PRECISION && p0[1] < exposure_array[i].pos[1]+0.5*exposure_array[i].v[1]-GCODE_PRECISION)
            remove = 1;
        }
      }

      if (remove)
      {
        index2_block = index1_block;
        index1_block = index1_block->next;
        gcode_list_remove (index2_block);
      }
      else
      {
        index1_block = index1_block->next;
      }
    }
  }


  /* Distance from Trace Check */
  for (i = 0; i < trace_num; i++)
  {
    line->p0[0] = trace_array[i].p0[0];
    line->p0[1] = trace_array[i].p0[1];
    line->p1[0] = trace_array[i].p1[0];
    line->p1[1] = trace_array[i].p1[1];

    index1_block = sketch->list;
    while (index1_block) /* xxx - add remove flag to optimize and see if there is speed improvement */
    {
      gcode_vec2d_t midpt;
      remove = 0;

      index1_block->ends (index1_block, pos[0], pos[1], GCODE_GET);

      /* Intersect Test 1 - does end pt fall within trace domain and is it less than aperture radius. */
      SOLVE_U (line->p0, line->p1, pos[0], u);
      if (u > 0.0+GCODE_PRECISION && u < 1.0-GCODE_PRECISION)
      {
        dpos[0] = line->p0[0] + u * (line->p1[0] - line->p0[0]);
        dpos[1] = line->p0[1] + u * (line->p1[1] - line->p0[1]);
        dist = sqrt ((dpos[0]-pos[0][0])*(dpos[0]-pos[0][0]) + (dpos[1]-pos[0][1])*(dpos[1]-pos[0][1]));
        if (dist < trace_array[i].radius-GCODE_PRECISION)
          remove = 1;
      }

      /* Intersect Test 2 - does end pt fall within trace domain and is it less than aperture radius. */
      SOLVE_U (line->p0, line->p1, pos[1], u);
      if (u > 0.0+GCODE_PRECISION && u < 1.0-GCODE_PRECISION)
      {
        dpos[0] = line->p0[0] + u * (line->p1[0] - line->p0[0]);
        dpos[1] = line->p0[1] + u * (line->p1[1] - line->p0[1]);
        dist = sqrt ((dpos[0]-pos[1][0])*(dpos[0]-pos[1][0]) + (dpos[1]-pos[1][1])*(dpos[1]-pos[1][1]));
        if (dist < trace_array[i].radius-GCODE_PRECISION)
          remove = 1;
      }

      /* Intersect Test 3 - Check End Points to see if they fall within the trace end radius */
      if (sqrt ((line->p0[0]-pos[0][0])*(line->p0[0]-pos[0][0]) + (line->p0[1]-pos[0][1])*(line->p0[1]-pos[0][1])) < trace_array[i].radius-GCODE_PRECISION)
        remove = 1;
      if (sqrt ((line->p0[0]-pos[1][0])*(line->p0[0]-pos[1][0]) + (line->p0[1]-pos[1][1])*(line->p0[1]-pos[1][1])) < trace_array[i].radius-GCODE_PRECISION)
        remove = 1;
      if (sqrt ((line->p1[0]-pos[0][0])*(line->p1[0]-pos[0][0]) + (line->p1[1]-pos[0][1])*(line->p1[1]-pos[0][1])) < trace_array[i].radius-GCODE_PRECISION)
        remove = 1;
      if (sqrt ((line->p1[0]-pos[1][0])*(line->p1[0]-pos[1][0]) + (line->p1[1]-pos[1][1])*(line->p1[1]-pos[1][1])) < trace_array[i].radius-GCODE_PRECISION)
        remove = 1;

      /* Intersect Test 4 - Lines only - Check if lines pass through the trace end radius */
      if (index1_block->type == GCODE_TYPE_LINE)
      {
        midpt[0] = (pos[0][0] + pos[1][0]) * 0.5;
        midpt[1] = (pos[0][1] + pos[1][1]) * 0.5;
        if (sqrt ((line->p0[0]-midpt[0])*(line->p0[0]-midpt[0]) + (line->p0[1]-midpt[1])*(line->p0[1]-midpt[1])) < trace_array[i].radius-GCODE_PRECISION)
          remove = 1;
        if (sqrt ((line->p1[0]-midpt[0])*(line->p1[0]-midpt[0]) + (line->p1[1]-midpt[1])*(line->p1[1]-midpt[1])) < trace_array[i].radius-GCODE_PRECISION)
          remove = 1;
      }

      /* Intersect Test 5 - Arcs only - Check if arcs pass through the trace */
      if (index1_block->type == GCODE_TYPE_ARC)
      {
        gcode_vec2d_t origin, center, p0;
        gfloat_t arc_radius_offset, start_angle;

        arc = (gcode_arc_t *) index1_block->pdata;

        gcode_arc_with_offset (index1_block, origin, center, p0, &arc_radius_offset, &start_angle);

        p0[0] = center[0] + arc->radius * cos (GCODE_DEG2RAD * (start_angle + arc->sweep * 0.5));
        p0[1] = center[1] + arc->radius * sin (GCODE_DEG2RAD * (start_angle + arc->sweep * 0.5));
        SOLVE_U (line->p0, line->p1, p0, u);
        if (u > 0.0 + GCODE_PRECISION && u < 1.0 - GCODE_PRECISION)
        {
          dpos[0] = line->p0[0] + u * (line->p1[0] - line->p0[0]);
          dpos[1] = line->p0[1] + u * (line->p1[1] - line->p0[1]);
          dist = sqrt ((dpos[0]-p0[0])*(dpos[0]-p0[0]) + (dpos[1]-p0[1])*(dpos[1]-p0[1]));

          if (dist < trace_array[i].radius-GCODE_PRECISION)
            remove = 1;
        }
      }

      if (remove)
      {
        index2_block = index1_block;
        index1_block = index1_block->next;
        gcode_list_remove (index2_block);
      }
      else
      {
        index1_block = index1_block->next;
      }
    }
  }

  line_block->free (&line_block);
}


static void
gcode_gerber_pass5 (gcode_block_t *sketch_block)
{
  /*
  * PASS 5 - Remove exact overlaps of segments as the result of two pads or traces being perfectly adjacent.
  * If an overlap occurs only remove one of the two, not both.
  */
  gcode_sketch_t *sketch;
  gcode_block_t *index1_block, *index2_block;
  gcode_vec2d_t e0[2], e1[2];
  gfloat_t dist0, dist1;
  int match;

  sketch = (gcode_sketch_t *) sketch_block->pdata;

  index1_block = sketch->list;
  while (index1_block)
  {
    index1_block->ends (index1_block, e0[0], e0[1], GCODE_GET);
    match = 0;

    index2_block = index1_block->next;
    while (index2_block && !match)
    {
      index2_block->ends (index2_block, e1[0], e1[1], GCODE_GET);
      dist0 = sqrt ((e0[0][0] - e1[0][0])*(e0[0][0] - e1[0][0]) + (e0[0][1] - e1[0][1])*(e0[0][1] - e1[0][1]));
      dist1 = sqrt ((e0[1][0] - e1[1][0])*(e0[1][0] - e1[1][0]) + (e0[1][1] - e1[1][1])*(e0[1][1] - e1[1][1]));
      if (dist0 < GCODE_PRECISION && dist1 < GCODE_PRECISION)
        match = 1;

      dist0 = sqrt ((e0[1][0] - e1[0][0])*(e0[1][0] - e1[0][0]) + (e0[1][1] - e1[0][1])*(e0[1][1] - e1[0][1]));
      dist1 = sqrt ((e0[0][0] - e1[1][0])*(e0[0][0] - e1[1][0]) + (e0[0][1] - e1[1][1])*(e0[0][1] - e1[1][1]));
      if (dist0 < GCODE_PRECISION && dist1 < GCODE_PRECISION)
        match = 1;

      /* Remove block2 */
      if (match)
      {
        gcode_list_remove (index2_block);
      }
      else
      {
        index2_block = index2_block->next;
      }
    }
    index1_block = index1_block->next;
  }
}


static void
gcode_gerber_pass6 (gcode_block_t *sketch_block)
{
  /*
  * PASS 6 - Correct the orientation and sequence of all segments.
  */
  gcode_sketch_t *sketch;

  sketch = (gcode_sketch_t *) sketch_block->pdata;

  gcode_util_order_list (sketch->list);
}


static void
gcode_gerber_pass7 (gcode_block_t *sketch_block)
{
  /*
  * PASS 7 - Merge adjacent lines with matching slopes.
  */
  gcode_sketch_t *sketch;
  gcode_block_t *index1_block, *index2_block;
  gcode_vec2d_t v0, v1, e0[2], e1[2];
  int merge;

  sketch = (gcode_sketch_t *) sketch_block->pdata;

  index1_block = sketch->list;
  while (index1_block && index1_block->next)
  {
    merge = 0;

    index1_block->ends (index1_block, e0[0], e0[1], GCODE_GET);
    GCODE_MATH_VEC2D_SUB (v0, e0[1], e0[0]);

    index2_block = index1_block->next;

    /* Check that both are lines */
    if (index1_block->type == GCODE_TYPE_LINE && index2_block->type == GCODE_TYPE_LINE)
    {
      index2_block->ends (index2_block, e1[0], e1[1], GCODE_GET);
      GCODE_MATH_VEC2D_SUB (v1, e1[1], e1[0]);

      /* Make sure the points are connected */
      if (fabs (e0[1][0] - e1[0][0]) < GCODE_PRECISION && fabs (e0[1][1] - e1[0][1]) < GCODE_PRECISION)
      {
        /* Check for matching slopes */
        merge = 0;
        if (fabs (v0[1]) < GCODE_PRECISION && fabs (v1[1]) < GCODE_PRECISION)
        {
          merge = 1;
/*          printf ("merging adjacent lines 1\n"); */
        }
        else if (fabs (v0[1]/v0[0] - v1[1]/v1[0]) < GCODE_PRECISION) /* XXX - Optimize by comparring inverted slopes or just compare values directly */
        {
          merge = 1;
/*          printf ("merging adjacent lines 2\n"); */
        }

        if (merge)
        {
          gcode_line_t *line;

          line = (gcode_line_t *) index1_block->pdata;
/* printf ("changing line from: %f,%f .. %f,%f\n", line->p0[0], line->p0[1], line->p1[0], line->p1[1]);*/
          line->p1[0] = e1[1][0];
          line->p1[1] = e1[1][1];
/* printf ("changing line to: %f,%f .. %f,%f\n", line->p0[0], line->p0[1], line->p1[0], line->p1[1]);*/
        }
      }
    }


    if (merge)
    {
      gcode_list_remove (index2_block);
      index1_block = sketch->list;
    }
    else
    {
      index1_block = index1_block->next;
    }
  }
}


int
gcode_gerber_import (gcode_block_t *sketch_block, char *filename, gfloat_t offset)
{
  FILE *fh;
  int trace_elbow_num, trace_num, exposure_num, error;
  gcode_vec3d_t *trace_elbow = NULL;
  gcode_sketch_t *sketch;
  gcode_gerber_trace_t *trace_array = NULL;
  gcode_gerber_exposure_t *exposure_array = NULL;

  fh = fopen (filename, "r");
  if (!fh)
    return (1);

  sketch = (gcode_sketch_t *) sketch_block->pdata;
  ((gcode_extrusion_t *) sketch->extrusion->pdata)->resolution = sketch_block->gcode->material_size[2];

  /* Set the comment as the file being opened */
  sprintf (sketch_block->comment, "offset: %.4f", offset);

  ((gcode_extrusion_t *) sketch->extrusion->pdata)->cut_side = GCODE_EXTRUSION_ALONG;

  trace_elbow_num = 0;
  trace_num = 0;
  exposure_num = 0;


  error = gcode_gerber_pass1 (sketch_block, fh, &trace_num, &trace_array, &trace_elbow_num, &trace_elbow, &exposure_num, &exposure_array, offset);

  if (error)
    return (1);

  gcode_gerber_pass2 (sketch_block, trace_elbow_num, trace_elbow);
  gcode_gerber_pass3 (sketch_block);
  gcode_gerber_pass4 (sketch_block, trace_num, trace_array, exposure_num, exposure_array);
  gcode_gerber_pass5 (sketch_block);
  gcode_gerber_pass6 (sketch_block);
/*  gcode_gerber_pass7 (sketch_block); */

  free (trace_array);
  free (exposure_array);
/*
  sprintf (mesg, "%d traces and %d pads\n", trace_num, exposure_num);
  sketch_block->gcode->message_callback (mesg);
*/

  fclose (fh);
  return (0);
}
