# Hardware-free unit tests.
#
#   make -C tests check        build and run with the host compiler
#   make -C tests check-asan   same, with AddressSanitizer and UBSan
#
# test_qdma:    the QDMA access layer (../qdma_access) against the userspace
#               shims in shim/ and the fake BAR in fake_mmio.c
# test_rx_poll: the real onic_netdev.c RX NAPI poll against drvshim/
# test_hw:      the real onic_hardware.c probe setup, teardown and error
#               interrupt handling against drvshim/ and the fake BARs
# test_reg_dump.py: tools/qdma_reg_dump.py against BAR images programmed by
#               the driver's own CSR code (gen_bar_image)

CC ?= cc
PYTHON ?= python3
TOP := ..

QDMA_SRCS := $(TOP)/qdma_access/qdma_context.c \
	$(TOP)/qdma_access/qdma_csr.c \
	$(TOP)/qdma_access/qdma_export.c \
	$(TOP)/qdma_access/qdma_version.c
SRCS := test_qdma.c fake_mmio.c legacy_csr_ref.c $(QDMA_SRCS)
GEN_SRCS := gen_bar_image.c fake_mmio.c $(QDMA_SRCS)
RX_SRCS := test_rx_poll.c drvshim/drvshim.c fake_mmio.c \
	$(TOP)/onic_netdev.c $(QDMA_SRCS)
HW_SRCS := test_hw.c drvshim/drvshim.c fake_mmio.c \
	$(TOP)/onic_hardware.c $(TOP)/qdma_access/qdma_device.c $(QDMA_SRCS)

HDRS := $(wildcard *.h shim/*.h shim/*/*.h drvshim/*.h drvshim/*/*.h \
	drvshim/*/*/*.h $(TOP)/*.h $(TOP)/qdma_access/*.h)

CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
	-Wno-sign-compare -Wno-missing-field-initializers \
	-I. -Ishim -I$(TOP) -I$(TOP)/qdma_access

# Driver sources are kernel code: kernel-like warnings and inline semantics
DRV_CFLAGS := -std=gnu11 -O1 -g -Wall -Werror -Wno-pointer-sign \
	-fgnu89-inline -include drvshim.h \
	-Idrvshim -I. -Ishim -I$(TOP) -I$(TOP)/qdma_access

ASAN := -fsanitize=address,undefined -fno-sanitize-recover=all

PROGS := test_qdma test_rx_poll test_hw gen_bar_image

.PHONY: all check check-asan clean

all: $(PROGS)

test_qdma: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

test_qdma_asan: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) $(ASAN) -o $@ $(SRCS)

gen_bar_image: $(GEN_SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(GEN_SRCS)

test_rx_poll: $(RX_SRCS) $(HDRS)
	$(CC) $(DRV_CFLAGS) -o $@ $(RX_SRCS)

test_rx_poll_asan: $(RX_SRCS) $(HDRS)
	$(CC) $(DRV_CFLAGS) $(ASAN) -o $@ $(RX_SRCS)

test_hw: $(HW_SRCS) $(HDRS)
	$(CC) $(DRV_CFLAGS) -o $@ $(HW_SRCS)

test_hw_asan: $(HW_SRCS) $(HDRS)
	$(CC) $(DRV_CFLAGS) $(ASAN) -o $@ $(HW_SRCS)

check: $(PROGS)
	./test_qdma
	./test_rx_poll
	./test_hw
	$(PYTHON) -I test_reg_dump.py

check-asan: test_qdma_asan test_rx_poll_asan test_hw_asan
	./test_qdma_asan
	./test_rx_poll_asan
	./test_hw_asan

clean:
	rm -f $(PROGS) $(PROGS:%=%_asan)
