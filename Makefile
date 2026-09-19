CROSS_COMPILE = arm-fullhanv3-linux-uclibcgnueabi-

COMMON_FLAGS = -Wall -Werror -fno-aggressive-loop-optimizations -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
COMMON_FLAGS += -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
COMMON_FLAGS += -ffunction-sections -fdata-sections -ftree-vectorize -fPIC
COMMON_FLAGS += -DSOCKLEN_T=socklen_t -DLOCALE_NOT_USED=1 -DNO_OPENSSL=1

CFLAGS = $(COMMON_FLAGS)
CXXFLAGS = $(COMMON_FLAGS) -std=c++11

CC = $(CROSS_COMPILE)gcc
CXX = $(CROSS_COMPILE)g++
STRIP = $(CROSS_COMPILE)strip

ROOT_DIR = .
LIBS_DIR = $(ROOT_DIR)/lib/static
LIVE555_DIR = $(ROOT_DIR)/lib/live555

INC_DIR = -I$(ROOT_DIR)/inc/
INC_DIR += -I$(ROOT_DIR)/include/
INC_DIR += -I$(ROOT_DIR)/include/types/
INC_DIR += -I$(ROOT_DIR)/include/dsp/
INC_DIR += -I$(ROOT_DIR)/include/dsp_ext/
INC_DIR += -I$(ROOT_DIR)/include/isp/
INC_DIR += -I$(ROOT_DIR)/include/isp_ext/
INC_DIR += -I$(ROOT_DIR)/include/mpp/
INC_DIR += -I$(ROOT_DIR)/include/vicap/
INC_DIR += -I$(LIVE555_DIR)/liveMedia/include
INC_DIR += -I$(LIVE555_DIR)/groupsock/include
INC_DIR += -I$(LIVE555_DIR)/UsageEnvironment/include
INC_DIR += -I$(LIVE555_DIR)/BasicUsageEnvironment/include

CFLAGS += $(INC_DIR)
CXXFLAGS += $(INC_DIR)

TARGET = demo

C_SOURCES = $(wildcard ./src/*.c)
CXX_SOURCES = $(wildcard ./src/*.cpp)
C_OBJECTS = $(C_SOURCES:%.c=%.o)
CXX_OBJECTS = $(CXX_SOURCES:%.cpp=%.o)
OBJECTS = $(C_OBJECTS) $(CXX_OBJECTS)
OBJECTSEX = $(wildcard ./obj/*.o)

FH_MPP_LIBS = \
	$(LIBS_DIR)/libdsp.a \
	$(LIBS_DIR)/libdbi.a \
	$(LIBS_DIR)/libvb_mpi.a \
	$(LIBS_DIR)/libvmm.a \
	$(LIBS_DIR)/libmipi.a \
	$(LIBS_DIR)/libimx415_mipi.a \
	$(LIBS_DIR)/libisp.a \
	$(LIBS_DIR)/libispcore.a \
	$(LIBS_DIR)/libadvapi_osd.a \
	$(LIBS_DIR)/libadvapi.a

LIVE555_LIBS = \
	$(LIVE555_DIR)/libliveMedia.a \
	$(LIVE555_DIR)/libgroupsock.a \
	$(LIVE555_DIR)/libBasicUsageEnvironment.a \
	$(LIVE555_DIR)/libUsageEnvironment.a

demo : $(OBJECTS)
	@$(CXX) $(CXXFLAGS) -o $@ $(OBJECTS) $(OBJECTSEX) $(FH_MPP_LIBS) $(LIVE555_LIBS) -lpthread -lm -lrt -ldl -lstdc++ -rdynamic
	rm -rf $(ROOT_DIR)/src/*.o

clean:
	rm -rf $(ROOT_DIR)/src/*.o
	rm -rf $(TARGET)
