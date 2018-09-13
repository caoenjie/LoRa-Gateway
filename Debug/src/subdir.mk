################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../src/base64.c \
../src/jitqueue.c \
../src/loragw_spi.c \
../src/main.c \
../src/parson.c \
../src/sx1276-Hal.c \
../src/sx1276-LoRa.c \
../src/sx1276-LoRaMisc.c \
../src/sx1276.c \
../src/timersync.c 

OBJS += \
./src/base64.o \
./src/jitqueue.o \
./src/loragw_spi.o \
./src/main.o \
./src/parson.o \
./src/sx1276-Hal.o \
./src/sx1276-LoRa.o \
./src/sx1276-LoRaMisc.o \
./src/sx1276.o \
./src/timersync.o 

C_DEPS += \
./src/base64.d \
./src/jitqueue.d \
./src/loragw_spi.d \
./src/main.d \
./src/parson.d \
./src/sx1276-Hal.d \
./src/sx1276-LoRa.d \
./src/sx1276-LoRaMisc.d \
./src/sx1276.d \
./src/timersync.d 


# Each subdirectory must supply rules for building sources it contributes
src/%.o: ../src/%.c
	@echo 'Building file: $<'
	@echo 'Invoking: Cross GCC Compiler'
	/home/enjie/gcc-linaro-arm-linux-gnueabihf-4.9-2014.09_linux/bin/arm-linux-gnueabihf-gcc  -march=armv7-a -mfloat-abi=hard -mfpu=neon --sysroot=/home/enjie/gcc-linaro-arm-linux-gnueabihf-4.9-2014.09_linux/bin/../arm-linux-gnueabihf/libc -I/home/enjie/eclipse-workspace/LoRa_gw/inc/ -O0 -g3 -Wall -O2 -pipe -g -feliminate-unused-debug-types --std=gnu99 -I/home/enjie/gcc-linaro-arm-linux-gnueabihf-4.9-2014.09_linux/arm-linux-gnueabihf/libc/usr/include -c -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


