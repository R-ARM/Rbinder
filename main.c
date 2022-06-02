#include <stdio.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>

int outfd = -1;

pthread_mutex_t	outfd_mutex;
pthread_mutexattr_t attr;

struct input_device
{
	char *path;
	char *name;
	int infd;
	pthread_t thread;
	struct input_device *next;
};

void emit(int type, int code, int value)
{
	if(outfd < 0)
		return;

	struct input_event ev;

	ev.type = type;
	ev.code = code;
	ev.value = value;

	ev.time.tv_sec = 0;
	ev.time.tv_usec = 0;

	pthread_mutex_lock(&outfd_mutex);
	if(write(outfd, &ev, sizeof(ev)) < 0)
		perror("Failed writing input event");
	pthread_mutex_unlock(&outfd_mutex);
}

int constrain(int x, int min, int max)
{
	if(x > max)
		return max;
	if(x < min)
		return min;

	return x;
}

void mode_act()
{
	printf("stub for BTN_MODE\n");
}

int cur_volume = 0;
int old_volume = 0;

void setvolume(int level)
{
	char cmd[64];
	int ret;

	cur_volume = constrain(level, 0, 100);

	printf("setting volume to %d\n", cur_volume);

	sprintf(cmd, "/usr/bin/amixer sset Master %d%%", cur_volume);
	printf("running %s\n", cmd);

	ret = system(cmd);
	if(ret)
		printf("failed running %s: %d\n", cmd, ret);
}

void vol_up()
{
	printf("volume up\n");
	setvolume(cur_volume + 5);
}

void vol_dn()
{
	printf("volume down\n");
	setvolume(cur_volume - 5);
}

void *worker(void *data)
{
	struct input_device *my_device = (struct input_device*)data;

	bool useful = false;
	char types[EV_MAX];
	char codes[KEY_MAX/8 + 1];
	
	memset(types, 0, EV_MAX);
	memset(codes, 0, sizeof(codes));
	ioctl(my_device->infd, EVIOCGBIT(0, EV_MAX), &types);
	
	for(int i = 0; i < EV_MAX; i++)
	{
		if((types[0] >> i) & 1)
		{
			switch(i)
			{
				case EV_KEY:
				case EV_SW:
					ioctl(my_device->infd, EVIOCGBIT(EV_KEY, sizeof(codes)), &codes);
					
					// dividing by 8 because all values
					// are stored as bits, not bytes
					// and manually checking bit
					if((codes[KEY_VOLUMEDOWN / 8]) & 1)
						useful = true;
					if((codes[KEY_VOLUMEUP / 8]) & 2)
						useful = true;
					if((codes[KEY_F12 / 8]) & 1)
						useful = true;
					break;
			}
		}
	}

	unsigned short id[4];

	// usb keyboards are NOT our problem
	ioctl(my_device->infd, EVIOCGID, &id);
	if(id[ID_BUS] == BUS_USB)
		useful = false;

	// we shouldn't open same device multiple times
	// or if anyone else grabs it
	if(ioctl(my_device->infd, EVIOCGRAB, 1))
		goto out;

	if(useful)
		printf("device \"%s\" deemed useful \n", my_device->name);
	else
		goto out;

	bool vol_up_held = false;
	bool vol_down_held = false;

	int rd = 0;
	int i = 0;
	struct input_event ev[4];
	while(1)
	{
		rd = read(my_device->infd, ev, sizeof(struct input_event) * 4);
		if(rd > 0)
		{
			for(i = 0; i < rd / sizeof(struct input_event) * 4; i++)
			{

				switch(ev[i].type)
				{
				case EV_KEY:
					switch(ev[i].code)
					{
					case KEY_F12: // bigger button on aya neo next
						emit(EV_KEY, BTN_MODE, ev[i].value);
						emit(EV_SYN, SYN_REPORT, 0);
						if(ev[i].value == 1)
							mode_act();
						break;
					case KEY_VOLUMEUP:
						vol_up_held = !!ev[i].value;
						if(vol_up_held && vol_down_held)
						{
							emit(EV_KEY, BTN_MODE, 1);
							emit(EV_SYN, SYN_REPORT, 0);
							setvolume(old_volume);
							mode_act();
						}
						else
						{
							emit(EV_KEY, BTN_MODE, 0);
							emit(EV_SYN, SYN_REPORT, 0);
						}

						if(!vol_down_held)
						{
							if(ev[i].value != 0)
								vol_up();
							if(!vol_up_held)
								old_volume = cur_volume;
						}
						break;
					case KEY_VOLUMEDOWN:
						vol_down_held = !!ev[i].value;
						if(vol_up_held && vol_down_held)
						{
							emit(EV_KEY, BTN_MODE, 1);
							emit(EV_SYN, SYN_REPORT, 0);
							setvolume(old_volume);
							mode_act();
						}
						else
						{
							emit(EV_KEY, BTN_MODE, 0);
							emit(EV_SYN, SYN_REPORT, 0);
						}

						if(!vol_up_held)
						{
							if(ev[i].value != 0)
								vol_dn();
							if(!vol_up_held)
								old_volume = cur_volume;
						}
						break;
					}
					break;
				}
				//emit(ev[i].type, ev[i].code, ev[i].value);
			}
		}
	}

out:
	close(my_device->infd);
	free(data);
	return NULL;
}

void attach_node(struct input_device *head, struct input_device *new)
{
	head->next = new;
	new->next = 0;
}

int rescan_devices(struct input_device *head)
{
	struct input_device *tmpdev;
	char dev[20];
	char name[33];
	name[32] = '\0';
	int tmpfd;

	for(int i = 0; i < 64; i++)
	{
		sprintf(dev, "/dev/input/event%d", i);
		tmpfd = open(dev, O_RDONLY);
		if(tmpfd == -1)
		{
			if(errno == ENOENT)
				return 0; // no more devices
			else
				perror("Failed opening device\n");

		}
		if(ioctl(tmpfd, EVIOCGNAME(32), name) < 0)
			continue;

		tmpdev = calloc(1, sizeof(struct input_device));
		tmpdev->path = malloc(strlen(dev) + 1);
		strcpy(tmpdev->path, dev);
		tmpdev->name = malloc(strlen(name) + 1);
		strcpy(tmpdev->name, name);
		tmpdev->infd = tmpfd;
		tmpdev->next = head->next;
		head->next = tmpdev;
		pthread_create(&tmpdev->thread, NULL, worker, tmpdev);
	}
	return 0;
}

int main(void)
{
	int ret = 0;
	struct input_device *head = malloc(sizeof(struct input_device));
	head->next = 0;

	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutex_init(&outfd_mutex, &attr);

	outfd = open("/dev/uinput", O_WRONLY);
	if(outfd < 0)
		perror("Error opening /dev/uinput");

	struct uinput_setup usetup;

	ioctl(outfd, UI_SET_EVBIT, EV_KEY);

	ioctl(outfd, UI_SET_KEYBIT, BTN_MODE); // just for aya neo
	
	// maybe we should pretend to be xbox gamepad?
	memset(&usetup, 0, sizeof(usetup));
	usetup.id.bustype = BUS_VIRTUAL;
	usetup.id.vendor = 0x1234;
	usetup.id.product = 0x5678;
	strcpy(usetup.name, "Rbinder");

	ioctl(outfd, UI_DEV_SETUP, &usetup);
	ioctl(outfd, UI_DEV_CREATE);

	while(ret == 0)
	{
		ret = rescan_devices(head);
		sleep(10);
	}
	
	pthread_mutex_destroy(&outfd_mutex);

	return ret;
}
