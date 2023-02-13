#ifndef __9PFRONT_H__
#define __9PFRONT_H__

void *init_9pfront(unsigned int id, const char *mnt);
void shutdown_9pfront(void *dev);

#endif /* __9PFRONT_H__ */
