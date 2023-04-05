#include <sys/mman.h>
#include "hw.h"
#include "qdev.h"
#include "isa.h"

struct testdev {
    ISADevice dev;
    MemoryRegion iomem;
    CharDriverState *chr;
};

#define TYPE_TESTDEV "testdev"
#define TESTDEV(obj) \
     OBJECT_CHECK(struct testdev, (obj), TYPE_TESTDEV)

static void test_device_serial_write(void *opaque, uint32_t addr, uint32_t data)
{
    struct testdev *dev = opaque;
    uint8_t buf[1] = { data };

    if (dev->chr) {
        qemu_chr_fe_write(dev->chr, buf, 1);
    }
}

static void test_device_exit(void *opaque, uint32_t addr, uint32_t data)
{
    exit(data);
}

static uint32_t test_device_memsize_read(void *opaque, uint32_t addr)
{
    return ram_size;
}

static void test_device_irq_line(void *opaque, uint32_t addr, uint32_t data)
{
    struct testdev *dev = opaque;

    qemu_set_irq(isa_get_irq(&dev->dev, addr - 0x2000), !!data);
}

static uint32 test_device_ioport_data;

static void test_device_ioport_write(void *opaque, uint32_t addr, uint32_t data)
{
    test_device_ioport_data = data;
}

static uint32_t test_device_ioport_read(void *opaque, uint32_t addr)
{
    return test_device_ioport_data;
}

static void test_device_flush_page(void *opaque, uint32_t addr, uint32_t data)
{
    target_phys_addr_t len = 4096;
    void *a = cpu_physical_memory_map(data & ~0xffful, &len, 0);

    mprotect(a, 4096, PROT_NONE);
    mprotect(a, 4096, PROT_READ|PROT_WRITE);
    cpu_physical_memory_unmap(a, len, 0, 0);
}

static char *iomem_buf;

static uint32_t test_iomem_readb(void *opaque, target_phys_addr_t addr)
{
    return iomem_buf[addr];
}

static uint32_t test_iomem_readw(void *opaque, target_phys_addr_t addr)
{
    return *(uint16_t*)(iomem_buf + addr);
}

static uint32_t test_iomem_readl(void *opaque, target_phys_addr_t addr)
{
    return *(uint32_t*)(iomem_buf + addr);
}

static void test_iomem_writeb(void *opaque, target_phys_addr_t addr, uint32_t val)
{
    iomem_buf[addr] = val;
}

static void test_iomem_writew(void *opaque, target_phys_addr_t addr, uint32_t val)
{
    *(uint16_t*)(iomem_buf + addr) = val;
}

static void test_iomem_writel(void *opaque, target_phys_addr_t addr, uint32_t val)
{
    *(uint32_t*)(iomem_buf + addr) = val;
}

static const MemoryRegionOps test_iomem_ops = {
    .old_mmio = {
        .read = { test_iomem_readb, test_iomem_readw, test_iomem_readl, },
        .write = { test_iomem_writeb, test_iomem_writew, test_iomem_writel, },
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static int init_test_device(ISADevice *isa)
{
    struct testdev *dev = DO_UPCAST(struct testdev, dev, isa);

    register_ioport_write(0xf1, 1, 1, test_device_serial_write, dev);
    register_ioport_write(0xf4, 1, 4, test_device_exit, dev);
    register_ioport_read(0xd1, 1, 4, test_device_memsize_read, dev);
    register_ioport_read(0xe0, 1, 1, test_device_ioport_read, dev);
    register_ioport_write(0xe0, 1, 1, test_device_ioport_write, dev);
    register_ioport_read(0xe0, 1, 2, test_device_ioport_read, dev);
    register_ioport_write(0xe0, 1, 2, test_device_ioport_write, dev);
    register_ioport_read(0xe0, 1, 4, test_device_ioport_read, dev);
    register_ioport_write(0xe0, 1, 4, test_device_ioport_write, dev);
    register_ioport_write(0xe4, 1, 4, test_device_flush_page, dev);
    register_ioport_write(0x2000, 24, 1, test_device_irq_line, NULL);
    iomem_buf = g_malloc0(0x10000);
    memory_region_init_io(&dev->iomem, &test_iomem_ops, dev,
                          "testdev", 0x10000);
    memory_region_add_subregion(isa_address_space(&dev->dev), 0xff000000,
                                                  &dev->iomem);
    return 0;
}

static Property testdev_isa_properties[] = {
    DEFINE_PROP_CHR("chardev", struct testdev, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void testdev_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ISADeviceClass *k = ISA_DEVICE_CLASS(klass);

    k->init = init_test_device;
    dc->props = testdev_isa_properties;
}

static TypeInfo testdev_info = {
    .name           = "testdev",
    .parent         = TYPE_ISA_DEVICE,
    .instance_size  = sizeof(struct testdev),
    .class_init     = testdev_class_init,
};

static void testdev_register_types(void)
{
    type_register_static(&testdev_info);
}

type_init(testdev_register_types)
