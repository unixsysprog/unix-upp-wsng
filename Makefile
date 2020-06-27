#
#
# makefile for webserver
#

CC = gcc -Wall

wsng: wsng.o socklib.o wsng_util.o
	$(CC) -o wsng wsng.o socklib.o wsng_util.o

clean:
	rm -f wsng.o socklib.o core wsng_util.o
