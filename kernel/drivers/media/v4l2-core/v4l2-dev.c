// Minimal video_device core: centralize character-device and devtmpfs/sysfs
// registration instead of making every capture driver create /dev/videoN.

#include <kernel/types.h>
#include <kernel/videodev2.h>

int
video_register_device(struct video_device *vdev)
{
  if(vdev == 0 || vdev->registered || vdev->name == 0 ||
     vdev->devnode == 0 || vdev->fops == 0 || vdev->major < 0 ||
     vdev->minor < 0)
    return -1;
  if(register_chrdev(vdev->major, vdev->devnode,
                     (struct file_operations *)vdev->fops) < 0)
    return -1;
  vdev->dev.name = vdev->name;
  vdev->dev.class = "video4linux";
  vdev->dev.devnode = vdev->devnode;
  vdev->dev.dev_major = vdev->major;
  vdev->dev.dev_minor = vdev->minor;
  vdev->dev.driver_data = vdev;
  if(device_register(&vdev->dev) < 0)
    return -1;
  vdev->registered = 1;
  return 0;
}

int
video_unregister_device(struct video_device *vdev)
{
  if(vdev == 0 || !vdev->registered)
    return -1;
  // The current character core has no unregister_chrdev because built-in
  // majors live for the kernel lifetime.  Still remove the dynamic device
  // object so devtmpfs and sysfs follow driver detach correctly.
  if(device_unregister(&vdev->dev) < 0)
    return -1;
  vdev->registered = 0;
  return 0;
}
