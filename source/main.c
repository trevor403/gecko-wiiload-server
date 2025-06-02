#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <ogcsys.h>
#include <gccore.h>
#include "state.h"
#include "wiiload.h"

state_t state = { .quit = 0, };

int main(int argc, char **argv) {
	VIDEO_Init();
	PAD_Init();
	
	GXRModeObj *rmode = VIDEO_GetPreferredMode(NULL);
	void *framebuffer = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	console_init(framebuffer,20,20,rmode->fbWidth,rmode->xfbHeight,rmode->fbWidth*VI_DISPLAY_PIX_SZ);
	
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(framebuffer);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if(rmode->viTVMode&VI_NON_INTERLACE) VIDEO_WaitVSync();


	printf("\nGecko Server\n");
	printf("Waiting for wiiload...\n");

	WIILOADBusy();
	WIILOADLoad();

	return 0;
}
