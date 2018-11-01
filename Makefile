build: anime_upscaler.c
	cc -O3 -Werror -Wall -Wno-unused-variable anime_upscaler.c -o anime_upscaler

clean:
	rm ./anime_upscaler

all: build
