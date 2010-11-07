/*

    File: photorec.c

    Copyright (C) 1998-2008 Christophe GRENIER <grenier@cgsecurity.org>

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

#include <stdio.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>	/* unlink, ftruncate */
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#include <errno.h>
#include "types.h"
#include "common.h"
#include "fnctdsk.h"
#include "dir.h"
#include "filegen.h"
#include "photorec.h"
#include "ext2p.h"
#include "fatp.h"
#include "ntfsp.h"
#include "log.h"
#include "setdate.h"

/* #define DEBUG_FILE_FINISH */
/* #define DEBUG_UPDATE_SEARCH_SPACE */
/* #define DEBUG_FREE */

static void update_search_space(const file_recovery_t *file_recovery, alloc_data_t *list_search_space, alloc_data_t **new_current_search_space, uint64_t *offset, const unsigned int blocksize);
static void update_search_space_aux(alloc_data_t *list_search_space, uint64_t start, uint64_t end, alloc_data_t **new_current_search_space, uint64_t *offset);
static alloc_data_t *file_truncate(alloc_data_t *space, file_recovery_t *file, const unsigned int sector_size, const unsigned int blocksize);
static alloc_data_t *file_error(alloc_data_t *space, file_recovery_t *file, const unsigned int blocksize);
static void list_free_add(const file_recovery_t *file_recovery, alloc_data_t *list_search_space);
static void list_space_used(const file_recovery_t *file_recovery, const unsigned int sector_size);

static void list_space_used(const file_recovery_t *file_recovery, const unsigned int sector_size)
{
  struct td_list_head *tmp;
  uint64_t file_size=0;
  uint64_t file_size_on_disk=0;
  if(file_recovery->filename==NULL)
    return;
  log_info("%s\t",file_recovery->filename);
  td_list_for_each(tmp, &file_recovery->location.list)
  {
    const alloc_list_t *element=td_list_entry(tmp, alloc_list_t, list);
    file_size_on_disk+=(element->end-element->start+1);
    if(element->data>0)
    {
      log_info(" %lu-%lu", (unsigned long)(element->start/sector_size), (unsigned long)(element->end/sector_size));
      file_size+=(element->end-element->start+1);
    }
    else
    {
      log_info(" (%lu-%lu)", (unsigned long)(element->start/sector_size), (unsigned long)(element->end/sector_size));
    }
  }
  log_info("\n");
  /*
  log_trace("list file_size %lu, file_size_on_disk %lu\n",
      (unsigned long)file_size, (unsigned long)file_size_on_disk);
  log_trace("file_size %lu, file_size_on_disk %lu\n",
      (unsigned long)file_recovery->file_size, (unsigned long)file_recovery->file_size_on_disk);
   */
}

static void list_free_add(const file_recovery_t *file_recovery, alloc_data_t *list_search_space)
{
  struct td_list_head *search_walker = NULL;
#ifdef DEBUG_FREE
  log_trace("list_free_add %lu\n",(long unsigned)(file_recovery->location.start/512));
#endif
  td_list_for_each(search_walker, &list_search_space->list)
  {
    alloc_data_t *current_search_space;
    current_search_space=td_list_entry(search_walker, alloc_data_t, list);
    if(current_search_space->start < file_recovery->location.start && file_recovery->location.start < current_search_space->end)
    {
      alloc_data_t *new_free_space;
      new_free_space=(alloc_data_t*)MALLOC(sizeof(*new_free_space));
      new_free_space->start=file_recovery->location.start;
      new_free_space->end=current_search_space->end;
      new_free_space->file_stat=NULL;
      current_search_space->end=file_recovery->location.start-1;
      td_list_add(&new_free_space->list, search_walker);
    }
    if(current_search_space->start==file_recovery->location.start)
    {
      current_search_space->file_stat=file_recovery->file_stat;
      return ;
    }
  }
}

/*
 * new_current_search_space!=NULL
 * offset!=NULL
 */
static void update_search_space(const file_recovery_t *file_recovery, alloc_data_t *list_search_space, alloc_data_t **new_current_search_space, uint64_t *offset, const unsigned int blocksize)
{
  struct td_list_head *search_walker = NULL;
#ifdef DEBUG_UPDATE_SEARCH_SPACE
  log_trace("update_search_space\n");
  info_list_search_space(list_search_space, NULL, DEFAULT_SECTOR_SIZE, 0, 1);
#endif

  td_list_for_each(search_walker, &list_search_space->list)
  {
    struct td_list_head *tmp;
    alloc_data_t *current_search_space;
    current_search_space=td_list_entry(search_walker, alloc_data_t, list);
    if(current_search_space->start <= file_recovery->location.start &&
        file_recovery->location.start <= current_search_space->end)
    {
      *offset=file_recovery->location.start;
      *new_current_search_space=current_search_space;
      td_list_for_each(tmp, &file_recovery->location.list)
      {
	const alloc_list_t *element=td_list_entry(tmp, alloc_list_t, list);
        uint64_t end=(element->end-(element->start%blocksize)+blocksize-1+1)/blocksize*blocksize+(element->start%blocksize)-1;
        update_search_space_aux(list_search_space, element->start, end, new_current_search_space, offset);
      }
      return ;
    }
  }
}

void del_search_space(alloc_data_t *list_search_space, const uint64_t start, const uint64_t end)
{
  update_search_space_aux(list_search_space, start, end, NULL, NULL);
}

static void update_search_space_aux(alloc_data_t *list_search_space, const uint64_t start, const uint64_t end, alloc_data_t **new_current_search_space, uint64_t *offset)
{
  struct td_list_head *search_walker = NULL;
#ifdef DEBUG_UPDATE_SEARCH_SPACE
  log_trace("update_search_space_aux offset=%llu remove [%llu-%llu]\n",
      (long long unsigned)(offset==NULL?0:((*offset)/512)),
      (unsigned long long)(start/512),
      (unsigned long long)(end/512));
#endif
  if(start > end)
    return ;
  td_list_for_each_prev(search_walker, &list_search_space->list)
  {
    alloc_data_t *current_search_space;
    current_search_space=td_list_entry(search_walker, alloc_data_t, list);
#ifdef DEBUG_UPDATE_SEARCH_SPACE
    log_trace("update_search_space_aux offset=%llu remove [%llu-%llu] in [%llu-%llu]\n",
	(long long unsigned)(offset==NULL?0:((*offset)/512)),
        (unsigned long long)(start/512),
        (unsigned long long)(end/512),
        (unsigned long long)(current_search_space->start/512),
        (unsigned long long)(current_search_space->end/512));
#endif
    if(current_search_space->start==start)
    {
      const uint64_t pivot=current_search_space->end+1;
      if(end < current_search_space->end)
      { /* current_search_space->start==start end<current_search_space->end */
        if(offset!=NULL && new_current_search_space!=NULL &&
            current_search_space->start<=*offset && *offset<=end)
        {
          *new_current_search_space=current_search_space;
          *offset=end+1;
        }
        current_search_space->start=end+1;
        current_search_space->file_stat=NULL;
        return ;
      }
      /* current_search_space->start==start current_search_space->end<=end */
      if(offset!=NULL && new_current_search_space!=NULL &&
          current_search_space->start<=*offset && *offset<=current_search_space->end)
      {
        *new_current_search_space=td_list_entry(current_search_space->list.next, alloc_data_t, list);
        *offset=(*new_current_search_space)->start;
      }
      td_list_del(search_walker);
      free(current_search_space);
      update_search_space_aux(list_search_space, pivot, end, new_current_search_space, offset);
      return ;
    }
    if(current_search_space->end==end)
    {
      const uint64_t pivot=current_search_space->start-1;
#ifdef DEBUG_UPDATE_SEARCH_SPACE
      log_trace("current_search_space->end==end\n");
#endif
      if(current_search_space->start < start)
      { /* current_search_space->start<start current_search_space->end==end */
        if(offset!=NULL && new_current_search_space!=NULL &&
            start<=*offset && *offset<=current_search_space->end)
        {
          *new_current_search_space=td_list_entry(current_search_space->list.next, alloc_data_t, list);
          *offset=(*new_current_search_space)->start;
        }
        current_search_space->end=start-1;
        return ;
      }
      /* start<=current_search_space->start current_search_space->end==end */
      if(offset!=NULL && new_current_search_space!=NULL &&
          current_search_space->start<=*offset && *offset<=current_search_space->end)
      {
        *new_current_search_space=td_list_entry(current_search_space->list.next, alloc_data_t, list);
        *offset=(*new_current_search_space)->start;
      }
      td_list_del(search_walker);
      free(current_search_space);
      update_search_space_aux(list_search_space, start, pivot, new_current_search_space, offset);
      return ;
    }
    if(start < current_search_space->start && current_search_space->start <= end)
    {
      const uint64_t pivot=current_search_space->start;
      update_search_space_aux(list_search_space, start, pivot-1,  new_current_search_space, offset);
      update_search_space_aux(list_search_space, pivot, end,      new_current_search_space, offset);
      return ;
    }
    if(start <= current_search_space->end && current_search_space->end < end)
    {
      const uint64_t pivot=current_search_space->end;
      update_search_space_aux(list_search_space, start, pivot, new_current_search_space, offset);
      update_search_space_aux(list_search_space, pivot+1, end, new_current_search_space, offset);
      return ;
    }
    if(current_search_space->start < start && end < current_search_space->end)
    {
      alloc_data_t *new_free_space;
      new_free_space=(alloc_data_t*)MALLOC(sizeof(*new_free_space));
      new_free_space->start=start;
      new_free_space->end=current_search_space->end;
      new_free_space->file_stat=NULL;
      current_search_space->end=start-1;
      td_list_add(&new_free_space->list,search_walker);
      if(offset!=NULL && new_current_search_space!=NULL &&
          new_free_space->start<=*offset && *offset<=new_free_space->end)
      {
        *new_current_search_space=new_free_space;
      }
      update_search_space_aux(list_search_space, start, end, new_current_search_space, offset);
      return ;
    }
  }
}

void init_search_space(alloc_data_t *list_search_space, const disk_t *disk_car, const partition_t *partition)
{
  alloc_data_t *new_sp;
  new_sp=(alloc_data_t*)MALLOC(sizeof(*new_sp));
  new_sp->start=partition->part_offset;
  new_sp->end=partition->part_offset+partition->part_size-1;
  if(new_sp->end > disk_car->disk_size-1)
    new_sp->end = disk_car->disk_size-1;
  if(new_sp->end > disk_car->disk_real_size-1)
    new_sp->end = disk_car->disk_real_size-1;
  new_sp->file_stat=NULL;
  new_sp->list.prev=&new_sp->list;
  new_sp->list.next=&new_sp->list;
  td_list_add_tail(&new_sp->list, &list_search_space->list);
}

void free_list_search_space(alloc_data_t *list_search_space)
{
  struct td_list_head *search_walker = NULL;
  struct td_list_head *search_walker_next = NULL;
  td_list_for_each_safe(search_walker,search_walker_next,&list_search_space->list)
  {
    alloc_data_t *current_search_space;
    current_search_space=td_list_entry(search_walker, alloc_data_t, list);
    td_list_del(search_walker);
    free(current_search_space);
  }
}

unsigned int photorec_mkdir(const char *recup_dir, const unsigned int initial_dir_num)
{
  char working_recup_dir[2048];
  int dir_ok=0;
  int dir_num=initial_dir_num;
#ifdef DJGPP
  int i=0;
#endif
  do
  {
    snprintf(working_recup_dir,sizeof(working_recup_dir)-1,"%s.%d",recup_dir,dir_num);
#ifdef HAVE_MKDIR
#ifdef __MINGW32__
    if(mkdir(working_recup_dir)!=0 && errno==EEXIST)
#else
      if(mkdir(working_recup_dir, 0775)!=0 && errno==EEXIST)
#endif
#else
#warning You need a mkdir function!
#endif
      {
	dir_num++;
      }
      else
      {
	dir_ok=1;
      }
#ifdef DJGPP
  /* Avoid endless loop in Dos version of Photorec after 999 directories if working with short name */
    i++;
    if(dir_ok==0 && i==1000)
    {
      dir_num=initial_dir_num;
      dir_ok=1;
    }
#endif
  } while(dir_ok==0);
  return dir_num;
}

int get_prev_file_header(alloc_data_t *list_search_space, alloc_data_t **current_search_space, uint64_t *offset)
{
  int nbr;
  alloc_data_t *file_space=*current_search_space;
  uint64_t size=0;
  /* Search backward the first fragment of a file not successfully recovered
   * Limit the search to 10 fragments and 1GB */
  for(nbr=0;nbr<10 && size<(uint64_t)1024*1024*1024;nbr++)
  {
    file_space=td_list_entry(file_space->list.prev, alloc_data_t, list);
    if(file_space==list_search_space)
      return -1;
    if(file_space->file_stat!=NULL)
    {
      *current_search_space=file_space;
      *offset=file_space->start;
      return 0;
    }
    size+=file_space->end - file_space->start + 1;
  }
  return -1;
}

void forget(alloc_data_t *list_search_space, alloc_data_t *current_search_space)
{
  struct td_list_head *search_walker = NULL;
  struct td_list_head *prev= NULL;
  int nbr=0;
  if(current_search_space==list_search_space)
    return ;
  for(search_walker=&current_search_space->list;
      search_walker!=&list_search_space->list;
      search_walker=prev)
  {
    prev=search_walker->prev;
    if(nbr>10000)
    {
      alloc_data_t *tmp;
      tmp=td_list_entry(search_walker, alloc_data_t, list);
      td_list_del(&tmp->list);
      free(tmp);
    }
    else
      nbr++;
  }
}

unsigned int remove_used_space(disk_t *disk_car, const partition_t *partition, alloc_data_t *list_search_space)
{
  if( partition->upart_type==UP_FAT12 ||
      partition->upart_type==UP_FAT16 || 
      partition->upart_type==UP_FAT32)
    return fat_remove_used_space(disk_car, partition, list_search_space);
#ifdef HAVE_LIBNTFS
  else if(partition->upart_type==UP_NTFS)
    return ntfs_remove_used_space(disk_car, partition, list_search_space);
#endif
#ifdef HAVE_LIBEXT2FS
  else if(partition->upart_type==UP_EXT2 || partition->upart_type==UP_EXT3 || partition->upart_type==UP_EXT4)
    return ext2_remove_used_space(disk_car, partition, list_search_space);
#endif
  return 0;
}

void update_stats(file_stat_t *file_stats, alloc_data_t *list_search_space)
{
  struct td_list_head *search_walker = NULL;
  int i;
  /* Reset */
  for(i=0;file_stats[i].file_hint!=NULL;i++)
    file_stats[i].not_recovered=0;
  /* Update */
  td_list_for_each(search_walker, &list_search_space->list)
  {
    alloc_data_t *current_search_space;
    current_search_space=td_list_entry(search_walker, alloc_data_t, list);
    if(current_search_space->file_stat!=NULL)
    {
      current_search_space->file_stat->not_recovered++;
    }
  }
}

void write_stats_log(const file_stat_t *file_stats)
{
  unsigned int file_nbr=0;
  unsigned int i;
  unsigned int nbr;
  file_stat_t *new_file_stats;
  for(i=0;file_stats[i].file_hint!=NULL;i++);
  if(i==0)
    return ;
  nbr=i;
  new_file_stats=(file_stat_t*)MALLOC(nbr*sizeof(file_stat_t));
  memcpy(new_file_stats, file_stats, nbr*sizeof(file_stat_t));
  qsort(new_file_stats, nbr, sizeof(file_stat_t), sorfile_stat_ts);
  for(i=0;i<nbr;i++)
  {
    if(new_file_stats[i].recovered+new_file_stats[i].not_recovered>0)
    {
      file_nbr+=new_file_stats[i].recovered;
      log_info("%s: %u/%u recovered\n",
          (new_file_stats[i].file_hint->extension!=NULL?
           new_file_stats[i].file_hint->extension:""),
          new_file_stats[i].recovered, new_file_stats[i].recovered+new_file_stats[i].not_recovered);
    }
  }
  free(new_file_stats);
  if(file_nbr>1)
  {
    log_info("Total: %u files found\n\n",file_nbr);
  }
  else
  {
    log_info("Total: %u file found\n\n",file_nbr);
  }
}

int sorfile_stat_ts(const void *p1, const void *p2)
{
  const file_stat_t *f1=(const file_stat_t *)p1;
  const file_stat_t *f2=(const file_stat_t *)p2;
  /* bigest to lowest */
  if(f1->recovered < f2->recovered)
    return 1;
  if(f1->recovered > f2->recovered)
    return -1;
  return 0;
}

void write_stats_stdout(const file_stat_t *file_stats)
{
  int i;
  unsigned int file_nbr=0;
  for(i=0;file_stats[i].file_hint!=NULL;i++)
  {
    if(file_stats[i].recovered+file_stats[i].not_recovered>0)
    {
      file_nbr+=file_stats[i].recovered;
      printf("%s: %u/%u recovered\n",
          (file_stats[i].file_hint->extension!=NULL?
           file_stats[i].file_hint->extension:""),
          file_stats[i].recovered, file_stats[i].recovered+file_stats[i].not_recovered);
    }
  }
  if(file_nbr>1)
  {
    printf("Total: %u files found\n\n",file_nbr);
  }
  else
  {
    printf("Total: %u file found\n\n",file_nbr);
  }
}

partition_t *new_whole_disk(const disk_t *disk_car)
{
  partition_t *fake_partition;
  fake_partition=partition_new(disk_car->arch);
  fake_partition->part_offset=0;
  fake_partition->part_size=disk_car->disk_size;
  strncpy(fake_partition->fsname,"Whole disk",sizeof(fake_partition->fsname)-1);
  return fake_partition;
}

unsigned int find_blocksize(alloc_data_t *list_search_space, const unsigned int default_blocksize, uint64_t *offset)
{
  unsigned int blocksize=128*512;
  struct td_list_head *search_walker = NULL;
  int run_again;
  *offset=0;
  if(td_list_empty(&list_search_space->list))
    return default_blocksize;
  *offset=(td_list_entry(list_search_space->list.next, alloc_data_t, list))->start % blocksize;
  do
  {
    run_again=0;
    td_list_for_each(search_walker, &list_search_space->list)
    {
      alloc_data_t *tmp;
      tmp=td_list_entry(search_walker, alloc_data_t, list);
      if(tmp->file_stat!=NULL)
      {
	if(tmp->start%blocksize!=*offset && blocksize>default_blocksize)
	{
	  blocksize=blocksize>>1;
	  *offset=tmp->start%blocksize;
	  run_again=1;
	}
      }
    }
  } while(run_again>0);
  return blocksize;
}

void update_blocksize(unsigned int blocksize, alloc_data_t *list_search_space, const uint64_t offset)
{
  struct td_list_head *search_walker = NULL;
  struct td_list_head *search_walker_next = NULL;
  td_list_for_each_safe(search_walker,search_walker_next,&list_search_space->list)
  {
    alloc_data_t *current_search_space;
    current_search_space=td_list_entry(search_walker, alloc_data_t, list);
    current_search_space->start=(current_search_space->start-offset%blocksize+blocksize-1)/blocksize*blocksize+offset%blocksize;
    if(current_search_space->start>current_search_space->end)
    {
      td_list_del(search_walker);
      free(current_search_space);
    }
  }
}

uint64_t free_list_allocation_end=0;

static void free_list_allocation(alloc_list_t *list_allocation)
{
  struct td_list_head *tmp = NULL;
  struct td_list_head *tmp_next = NULL;
  td_list_for_each_safe(tmp,tmp_next,&list_allocation->list)
  {
    alloc_list_t *allocated_space;
    allocated_space=td_list_entry(tmp, alloc_list_t, list);
    free_list_allocation_end=allocated_space->end;
    td_list_del(tmp);
    free(allocated_space);
  }
}

/* file_finish() returns
   -1: file not recovered, file_size=0 offset_error!=0
    0: file not recovered
    1: file recovered
 */
int file_finish(file_recovery_t *file_recovery, const char *recup_dir, const int paranoid, unsigned int *file_nbr,
    const unsigned int blocksize, alloc_data_t *list_search_space, alloc_data_t **current_search_space, uint64_t *offset,
    unsigned int *dir_num, const photorec_status_t status, const disk_t *disk)
{
  int file_recovered=0;
#ifdef DEBUG_FILE_FINISH
  log_debug("file_finish start %lu (%lu-%lu)\n", (long unsigned int)((*offset)/blocksize),
      (unsigned long int)((*current_search_space)->start/blocksize),
      (unsigned long int)((*current_search_space)->end/blocksize));
  log_debug("file_recovery->offset_error=%llu\n", (long long unsigned)file_recovery->offset_error);
  log_debug("file_recovery->handle %s NULL\n", (file_recovery->handle!=NULL?"!=":"=="));
  info_list_search_space(list_search_space, NULL, DEFAULT_SECTOR_SIZE, 0, 1);
#endif
  if(file_recovery->handle)
  {
    if(status!=STATUS_EXT2_ON_SAVE_EVERYTHING && status!=STATUS_EXT2_OFF_SAVE_EVERYTHING)
    {
      if(file_recovery->file_stat!=NULL && file_recovery->file_check!=NULL && paranoid>0)
      { /* Check if recovered file is valid */
        file_recovery->file_check(file_recovery);
      }
      /* FIXME: need to adapt read_size to volume size to avoid this */
      if(file_recovery->file_size > disk->disk_size)
        file_recovery->file_size = disk->disk_size;
      if(file_recovery->file_size > disk->disk_real_size)
        file_recovery->file_size = disk->disk_real_size;
      if(file_recovery->file_stat!=NULL && file_recovery->file_size> 0 &&
          file_recovery->file_size < file_recovery->min_filesize)
      { 
        log_info("File too small ( %llu < %llu), reject it\n",
            (long long unsigned) file_recovery->file_size,
            (long long unsigned) file_recovery->min_filesize);
        file_recovery->file_size=0;
        file_recovery->file_size_on_disk=0;
      }
#ifdef HAVE_FTRUNCATE
      fflush(file_recovery->handle);
      if(ftruncate(fileno(file_recovery->handle), file_recovery->file_size)<0)
      {
        log_critical("ftruncate failed.\n");
      }
#endif
    }
    fclose(file_recovery->handle);
    file_recovery->handle=NULL;
    //    log_debug("%s %llu\n",file_recovery->filename,(long long unsigned)file_recovery->file_size);
    if(file_recovery->file_size>0)
    {
      if(file_recovery->time!=0 && file_recovery->time!=(time_t)-1)
	set_date(file_recovery->filename, file_recovery->time, file_recovery->time);
      if((++(*file_nbr))%MAX_FILES_PER_DIR==0)
      {
        *dir_num=photorec_mkdir(recup_dir,*dir_num+1);
      }
      if(status!=STATUS_EXT2_ON_SAVE_EVERYTHING && status!=STATUS_EXT2_OFF_SAVE_EVERYTHING)
        file_recovery->file_stat->recovered++;
    }
    else
    {
      unlink(file_recovery->filename);
    }
  }
  if(file_recovery->file_stat!=NULL)
  {
    list_truncate(&file_recovery->location,file_recovery->file_size);
    if(file_recovery->file_size>0)
      list_space_used(file_recovery, disk->sector_size);
    if(file_recovery->file_size==0)
    {
      /* File hasn't been sucessfully recovered, remember where it begins */
      list_free_add(file_recovery, list_search_space);
      if((*current_search_space)!=list_search_space &&
          !((*current_search_space)->start <= *offset && *offset <= (*current_search_space)->end))
        *current_search_space=td_list_entry((*current_search_space)->list.next, alloc_data_t, list);
    }
    else if(status!=STATUS_EXT2_ON_SAVE_EVERYTHING && status!=STATUS_EXT2_OFF_SAVE_EVERYTHING && status!=STATUS_FIND_OFFSET)
    {
      update_search_space(file_recovery,list_search_space,current_search_space,offset,blocksize);
      file_recovered=1;
    }
    free_list_allocation(&file_recovery->location);
  }
  if(file_recovery->file_size==0 && file_recovery->offset_error!=0)
    file_recovered = -1;
  else
    reset_file_recovery(file_recovery);
#ifdef DEBUG_FILE_FINISH
  log_debug("file_finish end %lu (%lu-%lu)\n\n", (long unsigned int)((*offset)/blocksize),
      (unsigned long int)((*current_search_space)->start/blocksize),
      (unsigned long int)((*current_search_space)->end/blocksize));
  info_list_search_space(list_search_space, NULL, DEFAULT_SECTOR_SIZE, 0, 1);
#endif
  return file_recovered;
}

alloc_data_t *file_finish2(file_recovery_t *file_recovery, const char *recup_dir, const int paranoid, unsigned int *file_nbr,
    const unsigned int blocksize, alloc_data_t *list_search_space,
    unsigned int *dir_num, const photorec_status_t status, const disk_t *disk)
{
  alloc_data_t *datanext=NULL;
#ifdef DEBUG_FILE_FINISH
  log_debug("file_recovery->offset_error=%llu\n", (long long unsigned)file_recovery->offset_error);
  log_debug("file_recovery->handle %s NULL\n", (file_recovery->handle!=NULL?"!=":"=="));
  info_list_search_space(list_search_space, NULL, DEFAULT_SECTOR_SIZE, 0, 1);
#endif
  if(file_recovery->handle)
  {
    if(status!=STATUS_EXT2_ON_SAVE_EVERYTHING && status!=STATUS_EXT2_OFF_SAVE_EVERYTHING)
    {
      if(file_recovery->file_stat!=NULL && file_recovery->file_check!=NULL && paranoid>0)
      { /* Check if recovered file is valid */
        file_recovery->file_check(file_recovery);
      }
      /* FIXME: need to adapt read_size to volume size to avoid this */
      if(file_recovery->file_size > disk->disk_size)
        file_recovery->file_size = disk->disk_size;
      if(file_recovery->file_size > disk->disk_real_size)
        file_recovery->file_size = disk->disk_real_size;

      if(file_recovery->file_stat!=NULL && file_recovery->file_size> 0 &&
          file_recovery->file_size < file_recovery->min_filesize)
      { 
        log_info("File too small ( %llu < %llu), reject it\n",
            (long long unsigned) file_recovery->file_size,
            (long long unsigned) file_recovery->min_filesize);
        file_recovery->file_size=0;
        file_recovery->file_size_on_disk=0;
      }
#ifdef HAVE_FTRUNCATE
      fflush(file_recovery->handle);
      if(ftruncate(fileno(file_recovery->handle), file_recovery->file_size)<0)
      {
        log_critical("ftruncate failed.\n");
      }
#endif
    }
    fclose(file_recovery->handle);
    file_recovery->handle=NULL;
    //    log_debug("%s %llu\n",file_recovery->filename,(long long unsigned)file_recovery->file_size);
    if(file_recovery->file_size>0)
    {
      if(file_recovery->time!=0 && file_recovery->time!=(time_t)-1)
	set_date(file_recovery->filename, file_recovery->time, file_recovery->time);
      if(file_recovery->file_rename!=NULL)
	file_recovery->file_rename(file_recovery->filename);
      if((++(*file_nbr))%MAX_FILES_PER_DIR==0)
      {
        *dir_num=photorec_mkdir(recup_dir,*dir_num+1);
      }
      if(status!=STATUS_EXT2_ON_SAVE_EVERYTHING && status!=STATUS_EXT2_OFF_SAVE_EVERYTHING)
        file_recovery->file_stat->recovered++;
    }
    else
    {
      unlink(file_recovery->filename);
    }
  }
  if(file_recovery->file_stat!=NULL)
  {
    if(file_recovery->file_size==0)
    {
      /* File hasn't been sucessfully recovered */
      if(file_recovery->offset_error>0)
	datanext=file_error(list_search_space, file_recovery, blocksize);
    }
    else
    {
      datanext=file_truncate(list_search_space, file_recovery, disk->sector_size, blocksize);
    }
    free_list_allocation(&file_recovery->location);
  }
  if(file_recovery->file_size==0 && file_recovery->offset_error!=0)
  {
  }
  else
    reset_file_recovery(file_recovery);
#ifdef DEBUG_FILE_FINISH
  info_list_search_space(list_search_space, NULL, DEFAULT_SECTOR_SIZE, 0, 1);
#endif
  return datanext;
}

void info_list_search_space(const alloc_data_t *list_search_space, const alloc_data_t *current_search_space, const unsigned int sector_size, const int keep_corrupted_file, const int verbose)
{
  struct td_list_head *search_walker = NULL;
  unsigned long int nbr_headers=0;
  uint64_t sectors_with_unknown_data=0;
  td_list_for_each(search_walker,&list_search_space->list)
  {
    alloc_data_t *tmp;
    tmp=td_list_entry(search_walker, alloc_data_t, list);
    if(tmp->file_stat!=NULL)
    {
      nbr_headers++;
      tmp->file_stat->not_recovered++;
    }
    sectors_with_unknown_data+=(tmp->end-tmp->start+sector_size-1)/sector_size;
    if(verbose>0)
    {
      if(tmp==current_search_space)
        log_info("* ");
      log_info("%lu-%lu: %s\n",(long unsigned)(tmp->start/sector_size),
          (long unsigned)(tmp->end/sector_size),
          (tmp->file_stat!=NULL && tmp->file_stat->file_hint!=NULL?
           (tmp->file_stat->file_hint->extension?
            tmp->file_stat->file_hint->extension:""):
           "(null)"));
    }
  }
  log_info("%llu sectors contains unknown data, %lu invalid files found %s.\n",
      (long long unsigned)sectors_with_unknown_data, (long unsigned)nbr_headers,
      (keep_corrupted_file>0?"but saved":"and rejected"));
}

static alloc_data_t *file_truncate_aux(alloc_data_t *space, alloc_data_t *file, const uint64_t file_size, const unsigned int sector_size, const unsigned int blocksize)
{
  struct td_list_head *tmp;
  struct td_list_head *next;
  uint64_t size=0;
  const uint64_t file_size_on_disk=(file_size+blocksize-1)/blocksize*blocksize;
  for(tmp=&file->list, next=tmp->next; tmp!=&space->list; tmp=next, next=tmp->next)
  {
    alloc_data_t *element=td_list_entry(tmp, alloc_data_t, list);
    if(size >= file_size)
      return element;
    if(element->data>0)
    {
      if(size + (element->end-element->start+1) <= file_size_on_disk)
      {
	size=size + (element->end-element->start+1);
	log_info(" %lu-%lu", (unsigned long)(element->start/sector_size), (unsigned long)(element->end/sector_size));
	td_list_del(tmp);
	free(element);
      }
      else
      {
	log_info(" %lu-%lu",
	    (unsigned long)(element->start/sector_size),
	    (unsigned long)((element->start + file_size_on_disk - size - 1)/sector_size));
	element->start+=file_size_on_disk - size;
	element->file_stat=NULL;
	element->data=1;
	return element;
      }
    }
    else
    {
      log_info(" (%lu-%lu)", (unsigned long)(element->start/sector_size), (unsigned long)(element->end/sector_size));
      td_list_del(tmp);
      free(element);
    }
  }
  return space;
}

static alloc_data_t *file_truncate(alloc_data_t *space, file_recovery_t *file, const unsigned int sector_size, const unsigned int blocksize)
{
  alloc_data_t *spacenext;
  alloc_data_t *datanext;
  if(file->filename!=NULL)
    log_info("%s\t", file->filename);
  else
    log_info("?\t");
  spacenext=file_truncate_aux(space, file->loc, file->file_size, sector_size, blocksize);
  log_info("\n");
  datanext=td_list_entry(&spacenext->list.next, alloc_data_t, list);
  return datanext;
}

static alloc_data_t *file_error_aux(alloc_data_t *space, alloc_data_t *file, const uint64_t file_size, const unsigned int blocksize)
{
  struct td_list_head *tmp;
  struct td_list_head *next;
  uint64_t size=0;
  const uint64_t file_size_on_disk=file_size/blocksize*blocksize;
  for(tmp=&file->list, next=tmp->next; tmp!=&space->list; tmp=next, next=tmp->next)
  {
    alloc_data_t *element=td_list_entry(tmp, alloc_data_t, list);
    if(size >= file_size)
      return NULL;
    if(element->data>0)
    {
      if(size + (element->end-element->start+1) <= file_size_on_disk)
      {
	size=size + (element->end-element->start+1);
      }
      else if(file_size_on_disk > size)
      {
	if(element->file_stat==NULL)
	  return NULL;
	if(next!=&space->list)
	{
	  alloc_data_t *new_element;
	  new_element=td_list_entry(next, alloc_data_t, list);
	  if(element->end+1==new_element->start && new_element->file_stat==NULL)
	  {
	    log_info("GOT IT\n");
	    new_element->start-=file_size_on_disk - size;
	    element->end=new_element->start - 1;
	    return new_element;
	  }
	}
	{
	  alloc_data_t *new_element;
	  new_element=(alloc_data_t*)MALLOC(sizeof(*new_element));
	  memcpy(new_element, element, sizeof(*new_element));
	  new_element->start+=file_size_on_disk - size;
	  new_element->file_stat=NULL;
	  td_list_add(&new_element->list, &element->list);
	  element->end=new_element->start - 1;
	  return new_element;
	}
      }
    }
  }
  return NULL;
}

static alloc_data_t *file_error(alloc_data_t *space, file_recovery_t *file, const unsigned int blocksize)
{
  return file_error_aux(space, file->loc, file->offset_error, blocksize);
}

void free_search_space(alloc_data_t *list_search_space)
{
  struct td_list_head *search_walker = NULL;
  struct td_list_head *search_walker_next = NULL;
  td_list_for_each_safe(search_walker,search_walker_next,&list_search_space->list)
  {
    alloc_data_t *current_search_space;
    current_search_space=td_list_entry(search_walker, alloc_data_t, list);
    td_list_del(search_walker);
    free(current_search_space);
  }
}

void set_filename(file_recovery_t *file_recovery, const char *recup_dir, const unsigned int dir_num, const disk_t *disk, const partition_t *partition, const int broken)
{
  if(file_recovery->extension==NULL || file_recovery->extension[0]=='\0')
  {
    snprintf(file_recovery->filename,sizeof(file_recovery->filename)-1,"%s.%u/%c%07u",recup_dir,
	dir_num,(broken?'b':'f'),
	(unsigned int)((file_recovery->location.start-partition->part_offset)/disk->sector_size));
  }
  else
  {
    snprintf(file_recovery->filename,sizeof(file_recovery->filename)-1,"%s.%u/%c%07u.%s",recup_dir,
	dir_num, (broken?'b':'f'),
	(unsigned int)((file_recovery->location.start-partition->part_offset)/disk->sector_size), file_recovery->extension);
  }
}

