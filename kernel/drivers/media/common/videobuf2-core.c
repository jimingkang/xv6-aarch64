// Small videobuf2-style queue shared by video capture drivers.

#include <kernel/types.h>
#include <kernel/param.h>
#include "aarch64.h"
#include <kernel/defs.h>
#include <kernel/videodev2.h>

int
vb2_queue_init(struct vb2_queue *q, uint32 count, uint32 length)
{
  if(q == 0 || count == 0 || count > VB2_MAX_BUFFERS || length == 0)
    return -1;
  memset(q, 0, sizeof(*q));
  q->num_buffers = count;
  for(uint32 i = 0; i < count; i++){
    q->bufs[i].state = VB2_BUF_STATE_DEQUEUED;
    q->bufs[i].index = i;
    q->bufs[i].offset = i * length;
    q->bufs[i].length = length;
  }
  return 0;
}

void
vb2_queue_cancel(struct vb2_queue *q)
{
  if(q == 0)
    return;
  q->done_head = q->done_count = 0;
  for(uint32 i = 0; i < q->num_buffers; i++){
    q->bufs[i].state = VB2_BUF_STATE_DEQUEUED;
    q->bufs[i].bytesused = 0;
  }
}

int
vb2_find_free(struct vb2_queue *q)
{
  if(q)
    for(uint32 i = 0; i < q->num_buffers; i++)
      if(q->bufs[i].state == VB2_BUF_STATE_DEQUEUED)
        return i;
  return -1;
}

int
vb2_buffer_queue(struct vb2_queue *q, int index)
{
  if(q == 0 || index < 0 || index >= (int)q->num_buffers ||
     q->bufs[index].state != VB2_BUF_STATE_DEQUEUED)
    return -1;
  q->bufs[index].state = VB2_BUF_STATE_QUEUED;
  return 0;
}

int
vb2_buffer_active(struct vb2_queue *q, int index)
{
  if(q == 0 || index < 0 || index >= (int)q->num_buffers ||
     q->bufs[index].state != VB2_BUF_STATE_QUEUED)
    return -1;
  q->bufs[index].state = VB2_BUF_STATE_ACTIVE;
  return 0;
}

int
vb2_buffer_done(struct vb2_queue *q, int index, uint32 bytesused,
                uint32 sequence, uint64 timestamp_us)
{
  struct vb2_buffer *b;
  if(q == 0 || index < 0 || index >= (int)q->num_buffers ||
     q->done_count >= q->num_buffers)
    return -1;
  b = &q->bufs[index];
  if(b->state != VB2_BUF_STATE_ACTIVE)
    return -1;
  b->state = VB2_BUF_STATE_DONE;
  b->bytesused = bytesused;
  b->sequence = sequence;
  b->timestamp_us = timestamp_us;
  q->doneq[(q->done_head + q->done_count) % q->num_buffers] = index;
  q->done_count++;
  return 0;
}

int
vb2_dqbuf(struct vb2_queue *q)
{
  int index;
  if(q == 0 || q->done_count == 0)
    return -1;
  index = q->doneq[q->done_head];
  q->done_head = (q->done_head + 1) % q->num_buffers;
  q->done_count--;
  q->bufs[index].state = VB2_BUF_STATE_USER;
  return index;
}

int
vb2_buffer_release(struct vb2_queue *q, int index)
{
  if(q == 0 || index < 0 || index >= (int)q->num_buffers ||
     q->bufs[index].state != VB2_BUF_STATE_USER)
    return -1;
  q->bufs[index].state = VB2_BUF_STATE_DEQUEUED;
  q->bufs[index].bytesused = 0;
  return 0;
}

uint32
vb2_done_count(struct vb2_queue *q)
{
  return q ? q->done_count : 0;
}
