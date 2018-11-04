build: anime_upscaler.c expandable_buffer.h process_utils.h temp_files.h
	cc -O3 -Werror -Wall -Wno-unused-variable -g anime_upscaler.c -o anime_upscaler

clean:
	rm ./anime_upscaler ./anime-upscaler-temp*

all: build
