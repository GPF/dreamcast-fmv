TARGET = fmv_play.elf
OBJS = fmv_play.o dcfmv.o #profiler.o

all: rm-elf $(TARGET)

include $(KOS_BASE)/Makefile.rules

# KOS_CFLAGS += -Wfloat-conversion -Wdouble-promotion
clean: rm-elf
	-rm -f $(OBJS) 

rm-elf:
	-rm -f $(TARGET)

$(TARGET): $(OBJS)
	kos-cc -o  $(TARGET) $(OBJS)  -L$(KOS_BASE)/lib -llz4 -lsh4zam -lzstd 

run: $(TARGET)
	$(KOS_LOADER) $(TARGET)

dist: $(TARGET)
	-rm -f $(OBJS)
	$(KOS_STRIP) $(TARGET)