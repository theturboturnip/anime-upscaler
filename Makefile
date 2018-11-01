build: anime_upscaler.c
	cc -Werror -Wall -Wno-unused-variable anime_upscaler.c -o anime_upscaler

run: build
	./anime_upscaler

clean:
	rm ./anime_upscaler

all: build