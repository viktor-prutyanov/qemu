#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "qom/object.h"
#include "chardev/char.h"
#include "chardev/char-fe.h"
#include "exec/memory.h"

#define dprintf(fmt, ...) {}

extern AddressSpace address_space_memory;

#define MMIO_STUB_SIZE 0x100

struct MMIOStub {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    Chardev *chr;
    Chardev *master_chr;
    QemuThread master_thread;
};

#define TYPE_MMIO_STUB "mmio-stub"
OBJECT_DECLARE_SIMPLE_TYPE(MMIOStub, MMIO_STUB)

enum mmio_stub_op {
    MMIO_STUB_NOP,
    MMIO_STUB_HELLO,
    MMIO_STUB_READ,
    MMIO_STUB_WRITE,
    MMIO_STUB_IRQ
};

struct mmio_stub_msg_hdr {
    uint8_t op;
    uint8_t size;
    uint64_t addr;
    uint64_t val;
} QEMU_PACKED;

static uint64_t mmio_stub_read(void *opaque, hwaddr addr, unsigned len)
{
    MMIOStub *dev = opaque;
    struct mmio_stub_msg_hdr hdr = (struct mmio_stub_msg_hdr){
        .op = MMIO_STUB_READ,
        .addr = addr,
        .size = len,
    };
    uint64_t buf;
    ChardevClass *cc = CHARDEV_GET_CLASS(dev->chr);

    dprintf("%s: addr = 0x%lx, len = %u\n", __func__, addr, len);

    qemu_chr_write(dev->chr, (uint8_t *)&hdr, sizeof(hdr), true);
    cc->chr_sync_read(dev->chr, (uint8_t *)&buf, len);

    dprintf("%s: ok addr = 0x%lx, len = %u, val = 0x%lx\n", __func__, addr, len, buf);

    return buf;
}

static void mmio_stub_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned len)
{
    MMIOStub *dev = opaque;
    struct mmio_stub_msg_hdr hdr = (struct mmio_stub_msg_hdr){
        .op     = MMIO_STUB_WRITE,
        .addr   = addr,
        .size   = len,
        .val    = val,
    };
    uint8_t ok = 0;
    ChardevClass *cc = CHARDEV_GET_CLASS(dev->chr);

    dprintf("%s: addr = 0x%lx, len = %u, val = 0x%lx\n", __func__, addr, len, val);

    qemu_chr_write(dev->chr, (void *)&hdr, sizeof(hdr), true);
    cc->chr_sync_read(dev->chr, &ok, sizeof(ok));

    dprintf("%s: %s addr = 0x%lx, len = %u, val = 0x%lx\n", __func__, ok ? "ok" : "fail", addr, len, val);
}

static const MemoryRegionOps mmio_stub_ops = {
    .read = mmio_stub_read,
    .write = mmio_stub_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void *mmio_stub_master_work(void *arg)
{
    MMIOStub *s = (MMIOStub *)arg;
    ChardevClass *cc = CHARDEV_GET_CLASS(s->master_chr);
    struct mmio_stub_msg_hdr hdr;
    uint64_t buf;
    uint8_t ok;
    MemTxResult result;

    while (true) {
        cc->chr_sync_read(s->master_chr, (void *)&hdr, sizeof(hdr));
        if (hdr.op == MMIO_STUB_READ) {
            dprintf("mmio-stub: master read: addr = 0x%lx, len = %u\n", hdr.addr, hdr.size);
            result = address_space_rw(&address_space_memory, hdr.addr, MEMTXATTRS_UNSPECIFIED, (void *)&buf, hdr.size, false);
            ok = (result == MEMTX_OK);
            qemu_chr_write(s->master_chr, (void *)&buf, hdr.size, true);
            dprintf("mmio-stub: master read %s: addr = 0x%lx, len = %u, val = 0x%lx\n",
                    ok ? "ok" : "fail", hdr.addr, hdr.size, buf);
        } else if (hdr.op == MMIO_STUB_WRITE) {
            dprintf("mmio-stub: master write: addr = 0x%lx, len = %u, val = 0x%lx\n", hdr.addr, hdr.size, hdr.val);
            result = address_space_rw(&address_space_memory, hdr.addr, MEMTXATTRS_UNSPECIFIED, (void *)&hdr.val, hdr.size, true);
            ok = (result == MEMTX_OK);
            qemu_chr_write(s->master_chr, (void *)&ok, sizeof(ok), true);
            dprintf("mmio-stub: master write %s: addr = 0x%lx, len = %u, val = 0x%lx\n",
                    ok ? "ok" : "fail", hdr.addr, hdr.size, hdr.val);
        } else if (hdr.op == MMIO_STUB_IRQ) {
            dprintf("mmio-stub: master IRQ\n");
            qemu_set_irq(s->irq, 1);
        }
    }

    return NULL;
}

static void mmio_stub_init(Object *obj)
{
    MMIOStub *s = MMIO_STUB(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    struct mmio_stub_msg_hdr hdr = (struct mmio_stub_msg_hdr){
        .op = MMIO_STUB_HELLO,
    };

    memory_region_init_io(&s->iomem, OBJECT(s), &mmio_stub_ops, s,
                          "mmio-stub", MMIO_STUB_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);

    sysbus_init_irq(sbd, &s->irq);

    s->chr = qemu_chr_find("mmio-stub-chr");
    if (!s->chr) {
        dprintf("%s: no chardev\n", __func__);
        exit(1);
    }

    dprintf("mmio-stub: chr = %p\n", s->chr);

    qemu_chr_write(s->chr, (void *)&hdr, sizeof(hdr), true);

    s->master_chr = qemu_chr_find("mmio-stub-master-chr");
    if (!s->master_chr) {
        dprintf("%s: no MASTER chardev\n", __func__);
        exit(1);
    }

    dprintf("mmio-stub: master_chr = %p\n", s->master_chr);

    qemu_thread_create(&s->master_thread, "mmio-stub-master",
            mmio_stub_master_work, (void *)s, 0);
}

static void mmio_stub_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->desc = "mmio-stub";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo mmio_stub_info = {
    .name           = TYPE_MMIO_STUB,
    .parent         = TYPE_SYS_BUS_DEVICE,
    .instance_size  = sizeof(MMIOStub),
    .instance_init  = mmio_stub_init,
    .class_init     = mmio_stub_class_init,
};

static void mmio_stub_register_types(void)
{
    type_register_static(&mmio_stub_info);
}

type_init(mmio_stub_register_types)
