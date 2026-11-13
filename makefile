all:
	gcc -Wall -Wextra -O2 -o file_server_select file_server_select.c
	gcc -Wall -Wextra -O2 -o client client.c

clean:
	rm -f file_server_select client downloaded_*