################################################################################
# Automatically-generated file. Do not edit!
################################################################################

-include ../makefile.init

RM := rm -rf

# All of the sources participating in the build are defined here
-include sources.mk
-include src/subdir.mk
-include subdir.mk
-include objects.mk

ifneq ($(MAKECMDGOALS),clean)
ifneq ($(strip $(C_DEPS)),)
-include $(C_DEPS)
endif
endif

-include ../makefile.defs

# Add inputs and outputs from these tool invocations to the build variables 

# All Target
all: LoRa_gw

# Tool invocations
LoRa_gw: $(OBJS) $(USER_OBJS)
	@echo 'Building target: $@'
	@echo 'Invoking: Cross GCC Linker'
	/home/enjie/gcc-linaro-arm-linux-gnueabihf-4.9-2014.09_linux/bin/arm-linux-gnueabihf-gcc  -march=armv7-a -mfloat-abi=hard -mfpu=neon --sysroot=/home/enjie/gcc-linaro-arm-linux-gnueabihf-4.9-2014.09_linux/bin/../arm-linux-gnueabihf/libc -L/home/enjie/json/libcjson -Wl,-O1 -Wl,--hash-style=gnu -Wl,--as-needed -L/home/enjie/gcc-linaro-arm-linux-gnueabihf-4.9-2014.09_linux/arm-linux-gnueabihf/libc/usr/lib/arm-linux-gnueabihf -L/home/enjie/gcc-linaro-arm-linux-gnueabihf-4.9-2014.09_linux/arm-linux-gnueabihf/libc/lib/arm-linux-gnueabihf -o "LoRa_gw" $(OBJS) $(USER_OBJS) $(LIBS)
	@echo 'Finished building target: $@'
	@echo ' '

# Other Targets
clean:
	-$(RM) $(EXECUTABLES)$(OBJS)$(C_DEPS) LoRa_gw
	-@echo ' '

.PHONY: all clean dependents

-include ../makefile.targets
