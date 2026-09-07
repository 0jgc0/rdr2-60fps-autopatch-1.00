# Build autopatch60-oneshot.elf (PS5 payload, ps5-payload-sdk)
#   export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk   (o donde tengas el SDK)
#   make
PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := autopatch60-oneshot.elf

CFLAGS := -Wall -Werror -Os

all: $(ELF)

$(ELF): autopatch60-oneshot.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(ELF)
