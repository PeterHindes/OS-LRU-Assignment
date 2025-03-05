# CSCI3753 - FA23 PA8

CC = gcc
CFLAGS = -c -g -Wall -Wextra
LFLAGS = -g -Wall -Wextra

SUBMITFILES = pager-predict.c

.PHONY: all clean

all: test-predict

test-predict: simulator.o pager-predict.o programs.o
	$(CC) $(LFLAGS) $^ -o $@

simulator.o: simulator.c programs.o simulator.h
	$(CC) $(CFLAGS) $<

pager-predict.o: pager-predict.c simulator.h
	$(CC) $(CFLAGS) $<

clean:
	rm -f test-basic test-predict test-predict test-api
	rm -f simulator.o pager-predict.o
	rm -f *~
	rm -f *.csv
	rm -f *.pdf

submit: 
	@read -r -p "please enter your identikey username: " username; \
	tar -cvf PA8-$$username.txt $(SUBMITFILES)
