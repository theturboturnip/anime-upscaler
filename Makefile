build: anime_upscaler.c expandable_buffer.h
	cc -O3 -Werror -Wall -Wno-unused-variable anime_upscaler.c -o anime_upscaler

clean:
	rm ./anime_upscaler ./anime-upscaler-temp*

all: build
