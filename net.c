#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "platform.h"

#include "util.h"
#include "net.h"
#include "ip.h"

struct net_protocol {
  struct net_protocol *next;
  uint16_t type;
  struct queue_head queue;
  void (*handler)(const uint8_t *data, size_t len, struct net_device *dev);
};

struct net_protocol_queue_entry {
  struct net_device *dev;
  size_t len;
  uint8_t data[];
};

static struct net_device *devices;
static struct net_protocol *protocols;

struct net_device *
net_device_alloc(void)
{
  struct net_device *dev;

  dev = memory_alloc(sizeof(*dev));
  if (!dev) {
    errorf("memory_alloc() failure");
    return NULL;
  }

  return dev;
}

int
net_device_register(struct net_device *dev)
{
  static unsigned int index = 0;

  dev->index = index++;
  snprintf(dev->name, sizeof(dev->name), "net%d", dev->index);
  dev->next = devices;
  devices = dev;
  infof("registered, dev=%s, type=0x%04x", dev->name, dev->type);
  return 0;
}

static int
net_device_open(struct net_device *dev)
{
  if (NET_DEVICE_IS_UP(dev)) {
    errorf("already opened, dev=%s", dev->name);
    return -1;
  }

  if (dev->ops->open) {
    if (dev->ops->open(dev) == -1) {
      errorf("failure, dev=%s", dev->name);
      return -1;
    }
  }

  dev->flags |= NET_DEVICE_FLAG_UP;
  infof("dev=%s, state=%s", dev->name, NET_DEVICE_STATE(dev));
  return 0;
}

static int
net_device_close(struct net_device *dev)
{
  if (!NET_DEVICE_IS_UP(dev)) {
    errorf("not opened, dev=%s", dev->name);
    return -1;
  }

  if (dev->ops->close) {
    if (dev->ops->close(dev) == -1) {
      errorf("failure, dev=%s", dev->name);
      return -1;
    }
  }

  dev->flags &= ~NET_DEVICE_FLAG_UP; // 特定のフラグを落とすのは、特定のフラグの否定と AND をとればよい
  infof("dev=%s, state=%s", dev->name, NET_DEVICE_STATE(dev));
  return 0;
}

int
net_device_output(struct net_device *dev, uint16_t type, const uint8_t *data, size_t len, const void *dst)
{
  if (!NET_DEVICE_IS_UP(dev)) {
    errorf("not opened, dev=%s", dev->name);
    return -1;
  }

  if (len > dev->mtu) {
    errorf("too long, dev=%s, mtu=%u, len=%zu", dev->name, dev->mtu, len);
    return -1;
  }

  debugf("dev=%s, type=0x%04x, len=%zu", dev->name, type, len);
  debugdump(data, len);

  if (dev->ops->transmit(dev, type, data, len, dst) == -1) {
    errorf("device transmit failure, dev=%s, len=%zu", dev->name, len);
    return -1;
  }

  return 0;
}

int
net_protocol_register(uint16_t type, void (*handler)(const uint8_t *data, size_t len, struct net_device *dev))
{
  struct net_protocol *proto;

  for (proto=protocols; proto; proto=proto->next) {
    if (type == proto->type) {
      errorf("already registered, type=0x%04x", type);
      return -1;
    }
  }

  proto = memory_alloc(sizeof(*proto));
  if (!proto) {
    errorf("memory_alloc() failure");
    return -1;
  }
  proto->type = type;
  proto->handler = handler;
  proto->next = protocols;
  protocols = proto;

  infof("registered, type=0x%04x", type);
  return 0;
}

int
net_input_handler(uint16_t type, const uint8_t *data, size_t len, struct net_device *dev)
{
  // デバイスが受信したパケットをプロトコルスタックに渡す
  struct net_protocol *proto;
  struct net_protocol_queue_entry *entry;

  for (proto=protocols; proto; proto=proto->next) {
    if (proto->type == type) {
      // エントリを受信キューにエンキュー
      entry = memory_alloc(sizeof(*entry) + len);
      if (!entry) {
        errorf("memory_alloc() failure");
        return -1;
      }
      entry->dev = dev;
      entry->len = len;
      memcpy(entry->data, data, len);

      queue_push(&proto->queue, entry); // no need mutex?

      debugf("queue pushed (num:%u), dev=%s, type=0x%04x, len=%zu", proto->queue.num, dev->name, type, len);
      debugdump(data, len);
      intr_raise_irq(INTR_IRQ_SOFTIRQ);
      return 0;
    }
  }

  // unsupported protocol
  return 0;
}

int
net_softirq_handler(void)
{
  struct net_protocol *proto;
  struct net_protocol_queue_entry *entry;

  for (proto=protocols; proto; proto=proto->next) {
    while (1) {
      entry = queue_pop(&proto->queue);
      if (!entry) {
        break;
      }
      debugf("queue popped (num:%u), dev=%s, type=0x%04x, len=%zu", proto->queue.num, entry->dev->name, proto->type, entry->len);
      debugdump(entry->data, entry->len);
      proto->handler(entry->data, entry->len, entry->dev); // プロトコルの処理関数
      memory_free(entry);
    }
  }

  return 0;
}

int
net_run(void)
{
  struct net_device *dev;

  // 割り込み機構の起動
  if (intr_run() == -1) {
    errorf("intr_run() failure");
    return -1;
  }

  debugf("open all devices...");
  for (dev=devices; dev; dev=dev->next) {
    net_device_open(dev);
  }
  debugf("running...");

  return 0;
}

void
net_shutdown(void)
{
  struct net_device *dev;

  debugf("close all devices...");
  for (dev=devices; dev; dev=dev->next) {
    net_device_close(dev);
  }
  intr_shutdown();
  debugf("shutting down");

  return;
}

int
net_init(void)
{
  if (intr_init() == -1) {
    errorf("intr_init() failure");
    return -1;
  }
  if (ip_init() == -1) {
    errorf("ip_init() failure");
    return -1;
  }
  infof("initialized");
  return 0;
}
