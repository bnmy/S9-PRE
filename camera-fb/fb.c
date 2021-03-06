/* File: fb.c */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <getopt.h>             /* getopt_long() */
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

/***************************************************************************
 * global parameters
 ***************************************************************************/
int color=1;
int chain_out=0;

typedef void (*ProcessFunc)(uint8_t *, int, int);

/* Device Name like /dev/fb */
#define FBNAME	"/dev/fb0"

/* fixed screen information */
struct fb_fix_screeninfo fix_info;

/* configurable screen info */
struct fb_var_screeninfo var_info;

/* The frame buffer memory pointer */
uint8_t *framebuffer;

/*
 * Macro to pack the pixel based on the rgb bit offset.
 * We compute each color value based on bit length
 * and shift it to its corresponding offset in the pixel.
 * each color component is 8 bits long
 *
 * For example: Considering a RGB565, the formula will
 * expand as:-
 * Red len=5, off=11 : Green len=6, off=6 : Blue len=5, off=0
 * pixel_value = ((red >> (8 - 5) << 11)|
 *       ((green >> (8 - 6) << 6) |
 *      ((blue >> (8 - 5) << 0)
 */
#define RGB16(r,g,b) ( \
        (((r) >> (8-var_info.red.length)) << var_info.red.offset) | \
        (((g) >> (8-var_info.green.length)) << var_info.green.offset) | \
        (((b) >> (8-var_info.blue.length)) << var_info.blue.offset) \
)

#define SET_PIXEL16(x,y,color) (((uint16_t*)framebuffer)[(x)+(y)*fix_info.line_length/2]=(color))

#define RGB32(r,g,b) ((uint32_t)( (b) | ((g)<<8) | ((r)<<16) ))
#define SET_PIXEL32(x,y,color) (((uint32_t*)framebuffer)[(x)+(y)*fix_info.line_length/4]=(color))

/* function to a filled rectangle at position (x,y), width w and height h */
void rect_fill(int x,int y, int w, int h, uint32_t color)
{
	int i, j;
	for (i=0;i<w;i++) {
		for (j=0;j<h;j++) {
			SET_PIXEL32(x+i,y+j,color);
		}
	}
}

/***************************************************************************
 * decoding stuff
 ***************************************************************************/
#define bounds(m,M,x) ((x)>M ? M : (x)<(m) ? m : (x))

static uint32_t yuv_to_rgb_24(uint8_t y, uint8_t u, uint8_t v)
{
	int C,D,E,R,G,B;
	
	C=y-16;D=u-128;E=v-128;
	B=bounds(0,255,(298*C+409*E+128)>>8);
	G=bounds(0,255,(298*C-100*D-208*E+128)>>8);
	R=bounds(0,255,(298*C+516*D+128)>>8);
	
	return RGB32(R,G,B);
}

static void process_image_yuv420p(uint8_t * videoFrame, int width, int height)
{
	int    		x, y, xx, yy;
	uint8_t 	Y, U, V;
	int 		size=width*height;
	int 		i, j;
	
	int			offset_U = size;
	int 		offset_V = size + (size/4);
	
	x = 0;
	y = 0;
	
	if (chain_out) {
		int sz=size, l;
		while (sz) {
			l=write(STDOUT_FILENO, videoFrame, sz);
			sz-=l;
		}
	}
	
	// Si en Noir & Blanc
	/*
	if (!color) {

		for (i=0;i<width;i++) {
			for (j=0;j<height;j++) {
				Y = videoFrame[i + j*width];
				SET_PIXEL32(x+i,y+j,RGB32(Y, Y, Y));
			}	
		}
		
	} else { // Si en Couleur
		/// YUV 420 -> YUV 444
		xx = 0;
		yy = 0;
		for (i=0;i<width;i++) {
			yy = 0;
			for (j=0;j<height;j++) {
				Y = videoFrame[i + j*width];
				U = videoFrame[offset_U + xx + yy*(width/2)];
				V = videoFrame[offset_V + xx + yy*(width/2)];
				if(j%2==0)
					{yy = yy + 1;}
				SET_PIXEL32(x+i,y+j,yuv_to_rgb_24(Y, V, U));
			}	
			if(i%2==0)
				{xx = xx + 1;}
		}

	}*/
}

/***************************************************************************
 * main
 ***************************************************************************/
static void usage (FILE *fp, int argc, char **argv)
{
	fprintf (fp,
		"Usage: %s [options]\n\n"
		"Options:\n"
		"-w | --size    <640*480|       Video size\n"
		"                320*240>\n"
		"-c                             Show colored image\n"
		"-o                             Chain the incoming data to standard output\n"
		"-h | --help                    Print this message\n"
		"\n",
		argv[0]);
}

//needed to parse command line arguments with getopt_long
static const char short_options [] = "w:h:c:o";

//also needed to parse command line arguments with getopt_long
static const struct option
long_options [] = {
	{ "size", required_argument, NULL, 'w' },
	{ "color", no_argument,      NULL, 'c' },
	{ "output", no_argument,     NULL, 'o' },
	{ "help", no_argument,       NULL, 'h' },
	{ 0, 0, 0, 0 }
};

typedef enum {      
	PIX_FMT_YUV420P,
	PIX_FMT_RGB565,
	PIX_FMT_RGB32,
	PIX_FMT_YUV422,
	PIX_FMT_RAW12
} pix_fmt;


typedef struct {
	int			width;
	int 		height;
	int			factor;
	uint8_t *	frame;
	ProcessFunc	process;
} ImgReaderArgs;



int main(int argc, char *argv[])
{
    int size;

	int					fbd;			/* Frame buffer descriptor */

	int                 width  = 640;
	int                 height = 480;
	int                 index;
	int                 c;
	pix_fmt             pixel_format = PIX_FMT_YUV420P;
	ProcessFunc			proc_func=process_image_yuv420p;
	int					factor = 6;
	
	int					quit=0;
	
	for (;;) {
		c = getopt_long (argc, argv, short_options, long_options, &index);

		if (-1 == c)
			break; //no more arguments

		switch (c) 
		{
			case 0: // getopt_long() flag
				break;

			case 'w':
				if (strcmp(optarg,"640*480")==0) {
					fprintf(stderr, "window size 640*480\n");
					width=640;
					height=480;
				} else if (strcmp(optarg,"320*240")==0) {
					fprintf(stderr, "window size 320*240\n");
					width=320;
					height=240;
				} else {
					sscanf(optarg,"%d*%d",&width,&height);
					fprintf(stderr, "window size %d*%d\n",width,height);
				}
				break;

			case 'c':
				color = 1;
				break;
				
			case 'o':
				chain_out = 1;
				break;
				
			case 'h':
				usage (stderr, argc, argv);
				exit (EXIT_SUCCESS);

			default:
				usage (stderr, argc, argv);
				exit (EXIT_FAILURE);
		}
	}

    /* Open the framebuffer device in read write */
    fbd = open(FBNAME, O_RDWR);
    if (fbd < 0) {
        fprintf(stderr, "Unable to open %s.\n", FBNAME);
        return EXIT_FAILURE;
    }

    /* Do Ioctl. Retrieve fixed screen info. */
    if (ioctl(fbd, FBIOGET_FSCREENINFO, &fix_info) < 0) {
        fprintf(stderr, "get fixed screen info failed: %s\n",
        	strerror(errno));
        close(fbd);
        return EXIT_FAILURE;
    }

    /* Do Ioctl. Get the variable screen info. */
	if (ioctl(fbd, FBIOGET_VSCREENINFO, &var_info) < 0) {
        fprintf(stderr, "Unable to retrieve variable screen info: %s\n",
	    	strerror(errno));
        close(fbd);
        return EXIT_FAILURE;
    }

    /* Print some screen info currently available */
    fprintf(stderr, "Screen resolution: (%dx%d)\n",var_info.xres,var_info.yres);
	fprintf(stderr, "x offset, y offset : %d, %d\n",var_info.xoffset,var_info.yoffset);
    fprintf(stderr, "Line width in bytes %d\n", fix_info.line_length);
    fprintf(stderr, "bits per pixel : %d\n", var_info.bits_per_pixel);
    fprintf(stderr, "Red: length %d bits, offset %d\n",var_info.red.length,var_info.red.offset);
	fprintf(stderr, "Green: length %d bits, offset %d\n",var_info.red.length, var_info.green.offset);
    fprintf(stderr, "Blue: length %d bits, offset %d\n",var_info.red.length,var_info.blue.offset);
	/* Calculate the size to mmap */
	size=fix_info.line_length * var_info.yres;

	/* Now mmap the framebuffer. */
	framebuffer = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fbd,0);
    if (framebuffer == NULL) {
        fprintf(stderr, "mmap failed:\n");
        close(fbd);
        return EXIT_FAILURE;
    }

    fprintf(stderr, "framebuffer mmap address=%p\n", framebuffer);
    fprintf(stderr, "framebuffer size=%d bytes\n", size);

#if 0
    rect_fill(100,200,300,150, RGB32(0, 0, 0xFF));

#else    
    uint8_t * videoFrame = (uint8_t*) malloc (width*height*factor/4);
	if (!videoFrame) {
		fprintf(stderr, "could not allocate buffer for video frame (%d bytes required)\n", width*height*2);
		exit (EXIT_FAILURE);
	}
	
    while (1) {
		uint8_t *ptr=videoFrame;
		int ret;
		int size=width*height*factor/4;
		while(size) {
			if ((ret = read(STDIN_FILENO, ptr , size)) <= 0) {
				fprintf(stderr, "No more data to be read\n");
				quit = 1;
				break;
			}
			ptr+=ret;
			size-=ret;
		}
		
		if (quit) break;
		
		proc_func(videoFrame, width, height);
    }
#endif

    /* Release mmap. */
    munmap(framebuffer,0);
    close(fbd);
    return EXIT_SUCCESS;
}

