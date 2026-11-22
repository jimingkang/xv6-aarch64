#ifndef XV6_VIDEODEV2_H
#define XV6_VIDEODEV2_H

#include <kernel/device.h>
#include <uapi/videodev2.h>

#define VB2_MAX_BUFFERS 8

enum vb2_buffer_state {
  VB2_BUF_STATE_DEQUEUED = 0,
  VB2_BUF_STATE_QUEUED,
  VB2_BUF_STATE_ACTIVE,
  VB2_BUF_STATE_DONE,
  VB2_BUF_STATE_USER,
};

struct vb2_buffer {
  enum vb2_buffer_state state;
  uint32 index;
  uint32 offset;
  uint32 length;
  uint32 bytesused;
  uint32 sequence;
  uint64 timestamp_us;
};

// The driver owns the lock.  All vb2 helpers are intentionally lockless and
// must be called with that lock held, just like Linux's queue state helpers
// are called under the queue/driver lock.
struct vb2_queue {
  struct vb2_buffer bufs[VB2_MAX_BUFFERS];
  uint32 num_buffers;
  int doneq[VB2_MAX_BUFFERS];
  uint32 done_head;
  uint32 done_count;
};

int vb2_queue_init(struct vb2_queue *q, uint32 count, uint32 length);
void vb2_queue_cancel(struct vb2_queue *q);
int vb2_find_free(struct vb2_queue *q);
int vb2_buffer_queue(struct vb2_queue *q, int index);
int vb2_buffer_active(struct vb2_queue *q, int index);
int vb2_buffer_done(struct vb2_queue *q, int index, uint32 bytesused,
                    uint32 sequence, uint64 timestamp_us);
int vb2_dqbuf(struct vb2_queue *q);
int vb2_buffer_release(struct vb2_queue *q, int index);
uint32 vb2_done_count(struct vb2_queue *q);

struct video_device {
  char *name;
  char *devnode;
  int major;
  int minor;
  const struct file_operations *fops;
  void *drv_priv;
  struct device dev;
  int registered;
};

int video_register_device(struct video_device *vdev);
int video_unregister_device(struct video_device *vdev);
static inline void video_set_drvdata(struct video_device *vdev, void *data)
{
  vdev->drv_priv = data;
}
static inline void *video_get_drvdata(struct video_device *vdev)
{
  return vdev->drv_priv;
}

#endif
