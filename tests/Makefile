# Hardware-free unit tests for the QDMA access layer.
#
#   make -C tests check        build and run with the host compiler
#   make -C tests check-asan   same, with AddressSanitizer and UBSan
#
# The real driver sources in ../qdma_access are compiled against the
# userspace shims in shim/ and the fake BAR in fake_mmio.c.

CC ?= cc
TOP := ..
SRCS := test_qdma.c fake_mmio.c legacy_csr_ref.c \
	$(TOP)/qdma_access/qdma_context.c \
	$(TOP)/qdma_access/qdma_csr.c \
	$(TOP)/qdma_access/qdma_export.c \
	$(TOP)/qdma_access/qdma_version.c
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
	-Wno-sign-compare -Wno-missing-field-initializers \
	-I. -Ishim -I$(TOP) -I$(TOP)/qdma_access

.PHONY: check check-asan clean

test_qdma: $(SRCS) $(wildcard *.h shim/*.h shim/*/*.h $(TOP)/qdma_access/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

test_qdma_asan: $(SRCS) $(wildcard *.h shim/*.h shim/*/*.h $(TOP)/qdma_access/*.h)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all \
		-o $@ $(SRCS)

check: test_qdma
	./test_qdma

check-asan: test_qdma_asan
	./test_qdma_asan

clean:
	rm -f test_qdma test_qdma_asan
