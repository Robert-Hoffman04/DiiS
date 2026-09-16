#---------------------------------------------------------------------------------
# Clear the implicit built in rules
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

include $(DEVKITPPC)/wii_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# INCLUDES is a list of directories containing extra header files
#---------------------------------------------------------------------------------
TARGET		:=	$(notdir $(CURDIR))
BUILD		:=	build
SOURCES		:=	source source/metaspu \
				source/addons source/utils source/gekko_utils \
				source/jit source/harness source/gx
DATA		:=	data  
INCLUDES	:=

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------

# -fno-strict-aliasing / -fwrapv / -fno-aggressive-loop-optimizations:
# the DeSmuME core relies on type-punning, signed-overflow wraparound and
# out-of-bounds pointer arithmetic that was harmless under the 2012 GCC 4.x
# toolchain but is undefined behaviour that modern GCC (13+) exploits at -O3,
# miscompiling the GPU/renderer into a blank screen. These flags disable the
# offending assumptions and restore correct output. Do not remove them.
OPTFLAGS    =   -O3 -fno-strict-aliasing -fwrapv -fno-aggressive-loop-optimizations -flto=auto -funroll-loops

# TESTDEFS/TESTLDFLAGS: empty by default. Pass e.g.
#   make TESTDEFS="-DDESMUME_FORCE_CORE=1 -DDESMUME_FORCE_ROM"
# to skip the on-screen device/renderer picker and file browser and boot
# straight into sd:/DS/ROMS/test.nds for automated testing.

# JITDEFS: empty by default. Pass  make JITDEFS=-DDESMUME_JIT_ARM7  to compile
# the ARM7 trace-JIT infrastructure in source/jit/ (see
# desmumewii-arm7-jit-plan.md). Not yet wired into execution. Toggling this
# needs a  make clean  -- the flag is not tracked in the dependency files.
JITDEFS     ?=

CFLAGS	    =   -D__BIG_ENDIAN__ -DENABLE_PAIRED_SINGLE -g $(OPTFLAGS) -fsigned-char -Wall $(MACHDEP) $(INCLUDE) $(TESTDEFS) $(JITDEFS)
CXXFLAGS	=	$(CFLAGS)
# .S files (jit_trampoline.S) are built with -x assembler-with-cpp and see only
# CPPFLAGS/ASFLAGS, not CFLAGS -- carry JITDEFS through so the DESMUME_JIT_ARM7
# guard reaches the assembler too.
ASFLAGS	    =	$(JITDEFS)
LDFLAGS	    =	-g $(MACHDEP) $(OPTFLAGS) -Wl,-Map,$(notdir $@).map $(TESTLDFLAGS)

#---------------------------------------------------------------------------------
# any extra libraries we wish to link with the project
#---------------------------------------------------------------------------------
LIBS        := -lz -lfat -lwiiuse -lbte -lasnd -logc -lm -lwiikeyboard

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS)

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

#---------------------------------------------------------------------------------
# automatically build a list of object files for our project
#---------------------------------------------------------------------------------
CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
sFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES	:=	$(addsuffix .o,$(BINFILES)) \
					$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) \
					$(sFILES:.s=.o) $(SFILES:.S=.o) \
					$(CURDIR)/source/gekko_utils/ehcmodule.elf.o

#---------------------------------------------------------------------------------
# build a list of include paths
#---------------------------------------------------------------------------------
export INCLUDE	:=	$(foreach dir,$(INCLUDES), -iquote $(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					-I$(CURDIR)/$(BUILD) \
					-I$(LIBOGC_INC)

#---------------------------------------------------------------------------------
# build a list of library paths
#---------------------------------------------------------------------------------
export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
					-L$(LIBOGC_LIB)

export OUTPUT	:=	$(CURDIR)/$(TARGET)
.PHONY: $(BUILD) clean

#---------------------------------------------------------------------------------
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@make --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).dol

#---------------------------------------------------------------------------------
run:
	wiiload $(TARGET).dol


#---------------------------------------------------------------------------------
else

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).dol: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

#---------------------------------------------------------------------------------
# This rule links in binary data with the .png extension
#---------------------------------------------------------------------------------
%.png.o	:	%.png
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
# This rule links in binary data with the .ttf extension
#---------------------------------------------------------------------------------
%.ttf.o	:	%.ttf
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
# This rule links in binary data with the .mp3 extension
#---------------------------------------------------------------------------------
%.mp3.o	:	%.mp3
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
