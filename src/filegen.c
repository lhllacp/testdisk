/*

    File: filegen.c

    Copyright (C) 2007-2008 Christophe GRENIER <grenier@cgsecurity.org>
  
    This software is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
  
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
  
    You should have received a copy of the GNU General Public License along
    with this program; if not, write the Free Software Foundation, Inc., 51
    Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#include <stdio.h>
#include "types.h"
#include "common.h"
#include "filegen.h"

static  file_check_t file_check_plist={
  .list = TD_LIST_HEAD_INIT(file_check_plist.list)
};

file_check_list_t file_check_list={
    .list = TD_LIST_HEAD_INIT(file_check_list.list)
};

static int file_check_cmp(const struct td_list_head *a, const struct td_list_head *b)
{
  const file_check_t *fc_a=td_list_entry(a, const file_check_t, list);
  const file_check_t *fc_b=td_list_entry(b, const file_check_t, list);
  int res;
  if(fc_a->length==0 && fc_b->length!=0)
    return 1;
  if(fc_a->length!=0 && fc_b->length==0)
    return -1;
  res=fc_a->offset-fc_b->offset;
  if(res!=0)
    return res;
  return memcmp(fc_a->value,fc_b->value, (fc_a->length<=fc_b->length?fc_a->length:fc_b->length));
}

static void file_check_add_tail(file_check_t *file_check_new, file_check_list_t *pos)
{
  unsigned int i;
  file_check_list_t *newe=(file_check_list_t *)MALLOC(sizeof(*newe));
  newe->offset=file_check_new->offset;
  newe->has_value=(file_check_new->length==0?0:1);
  for(i=0;i<256;i++)
  {
    newe->file_checks[i].list.prev=&newe->file_checks[i].list;
    newe->file_checks[i].list.next=&newe->file_checks[i].list;
  }
  td_list_add_tail(&file_check_new->list, &newe->file_checks[file_check_new->length==0?0:file_check_new->value[0]].list);
  td_list_add_tail(&newe->list, &pos->list);
}

void register_header_check(const unsigned int offset, const unsigned char *value, const unsigned int length, 
  int (*header_check)(const unsigned char *buffer, const unsigned int buffer_size,
      const unsigned int safe_header_only, const file_recovery_t *file_recovery, file_recovery_t *file_recovery_new),
  file_stat_t *file_stat)
{
  file_check_t *file_check_new=(file_check_t *)MALLOC(sizeof(*file_check_new));
  file_check_new->value=value;
  file_check_new->length=length;
  file_check_new->offset=offset;
  file_check_new->header_check=header_check;
  file_check_new->file_stat=file_stat;
  td_list_add_sorted(&file_check_new->list, &file_check_plist.list, file_check_cmp);
}

static void index_header_check_aux(file_check_t *file_check_new)
{
  struct td_list_head *tmp;
  td_list_for_each(tmp, &file_check_list.list)
  {
    file_check_list_t *pos=td_list_entry(tmp, file_check_list_t, list);
    if(file_check_new->length>0)
    {
      if(pos->has_value==1)
      {
	if(pos->offset>=file_check_new->offset &&
	    pos->offset<=file_check_new->offset+file_check_new->length)
	{
	  return td_list_add_sorted(&file_check_new->list,
	      &pos->file_checks[file_check_new->value[pos->offset-file_check_new->offset]].list,
	      file_check_cmp);
	}
	if(pos->offset>file_check_new->offset)
	{
	  return file_check_add_tail(file_check_new, pos);
	}
      }
    }
    else
    {
      return td_list_add_sorted(&file_check_new->list,
	  &pos->file_checks[0].list,
	  file_check_cmp);
    }
  }
  file_check_add_tail(file_check_new, &file_check_list);
}

void index_header_check(void)
{
  struct td_list_head *tmp;
  struct td_list_head *next;
  td_list_for_each_prev_safe(tmp, next, &file_check_plist.list)
  {
    file_check_t *current_check;
    current_check=td_list_entry(tmp, file_check_t, list);
    td_list_del(tmp);
    index_header_check_aux(current_check);
  }
}

void free_header_check(void)
{
  struct td_list_head *tmpl;
  struct td_list_head *nextl;
  td_list_for_each_safe(tmpl, nextl, &file_check_list.list)
  {
    unsigned int i;
    struct td_list_head *tmp;
    struct td_list_head *next;
    file_check_list_t *pos=td_list_entry(tmpl, file_check_list_t, list);
    for(i=0;i<256;i++)
    {
      td_list_for_each_safe(tmp, next, &pos->file_checks[i].list)
      {
	file_check_t *current_check;
	current_check=td_list_entry(tmp, file_check_t, list);
	td_list_del(tmp);
	free(current_check);
      }
    }
    td_list_del(tmpl);
    free(pos);
  }
}

void file_allow_nl(file_recovery_t *file_recovery, const unsigned int nl_mode)
{
  unsigned char buffer[4096];
  int taille;
  if(fseek(file_recovery->handle, file_recovery->file_size,SEEK_SET)<0)
    return;
  taille=fread(buffer,1, 4096,file_recovery->handle);
  if(taille > 0 && buffer[0]=='\n' && (nl_mode&NL_BARENL)==NL_BARENL)
    file_recovery->file_size++;
  else if(taille > 1 && buffer[0]=='\r' && buffer[1]=='\n' && (nl_mode&NL_CRLF)==NL_CRLF)
    file_recovery->file_size+=2;
  else if(taille > 0 && buffer[0]=='\r' && (nl_mode&NL_BARECR)==NL_BARECR)
    file_recovery->file_size++;
}

