default:
	$(CROSS_COMPILE)$(CC) main.c -o rbinder $(CFLAGS) -lpthread -Wall -Wextra
