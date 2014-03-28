all: main.cpp

main.cpp:
	g++ -std=c++0x -O2 -o setdyndns main.cpp

install:
	cp setdyndns /usr/bin/setdyndns && \
	chown root:dyndns /usr/bin/setdyndns && \
	chmod u=rwxs,g=rx,o= /usr/bin/setdyndns
