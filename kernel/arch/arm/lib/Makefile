lib-y		:= backtrace.o changebit.o csumipv6.o csumpartial.o   \
		   csumpartialcopy.o csumpartialcopyuser.o clearbit.o \
		   delay.o findbit.o memchr.o memcpy.o		      \
		   memmove.o memset.o memzero.o setbit.o              \
		   strncpy_from_user.o strnlen_user.o                 \
		   strchr.o strrchr.o                                 \
		   testchangebit.o testclearbit.o testsetbit.o        \
		   ashldi3.o ashrdi3.o lshrdi3.o muldi3.o             \
		   ucmpdi2.o lib1funcs.o div64.o                      \
		   io-readsb.o io-writesb.o io-readsl.o io-writesl.o

mmu-y	:= clear_user.o copy_page.o getuser.o putuser.o

ifeq ($(CONFIG_PREEMPT),y)
  mmu-y	+= copy_from_user.o copy_to_user.o
else
ifneq ($(CONFIG_CPU_32v3),y)
  mmu-y	+= copy_from_user.o copy_to_user.o
else
  mmu-y	+= uaccess.o
endif
endif

lib-$(CONFIG_MMU) += $(mmu-y)

ifeq ($(CONFIG_CPU_32v3),y)
  lib-y	+= io-readsw-armv3.o io-writesw-armv3.o
else
  lib-y	+= io-readsw-armv4.o io-writesw-armv4.o
endif

lib-$(CONFIG_ARCH_RPC)		+= ecard.o io-acorn.o floppydma.o
lib-$(CONFIG_ARCH_L7200)	+= io-acorn.o
lib-$(CONFIG_ARCH_SHARK)	+= io-shark.o

$(obj)/csumpartialcopy.o:	$(obj)/csumpartialcopygeneric.S
$(obj)/csumpartialcopyuser.o:	$(obj)/csumpartialcopygeneric.S

ifeq ($(CONFIG_KERNEL_MODE_NEON),y)
  NEON_FLAGS			:= -mfloat-abi=softfp -mfpu=neon
  CFLAGS_xor-neon.o		+= $(NEON_FLAGS)
  lib-$(CONFIG_XOR_BLOCKS)	+= xor-neon.o
endif
