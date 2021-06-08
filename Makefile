
all: install

install:
	gcc seashell.c -o seashell
	
test:
	./seashell

clean:
	rm seashell.exe

