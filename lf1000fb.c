/*
 * drivers/video/lf1000fb.c
 *
 * Framebuffer support for the LF1000/Pollux SoC.
 * 
 * Enabled for Didj and Leapster Explorer by nirvous
 * 
 * Greetz and many thanks to zucchini for his valuable help and input, 
 * as well as to Claude, GrizzlyAdams, jburks, jkent, losinggeneration, 
 * MostAwesomeDude, NullMoogleCable, PhilKll, prpplague, ReggieUK, 
 * and everyone on #Didj (irc.freenode.org).	 
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/fb.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/cpufreq.h>
#include <linux/platform_device.h>

#include <asm/uaccess.h>
#include <asm/io.h>

#include <mach/platform.h>
#include <mach/mlc.h>


/*
 * Settings
 */
#define X_RESOLUTION		320
#define Y_RESOLUTION		240

#define PALETTE_CLEAR		0x80000000
#define MLCCONTROL0			0x24 //0x4024
#define MLCHSTRIDE0			0x28 //0x4028
#define MLCVSTRIDE0			0x2C //0x402C
#define MLCCONTROL1         0x58 //0x4058
#define MLCHSTRIDE1			0x5C //0x405C
#define MLCVSTRIDE1			0x60 //0x4060


#define MLCBGCOLOR			0x8 //0x4008
#define DIRTYFLAG			4
#define LAYERENB			5				
#define FORMAT				16	/* see table 20-5 */


//#define BGCOLOR				0xFFFFFF  //RGBCODE
#define BYTESPP				2
#define BITSPP				BYTESPP*8
#define VISUALTYPE			FB_VISUAL_TRUECOLOR //FB_VISUAL_PSEUDOCOLOR, FB_VISUAL_TRUECOLOR
#define FORMATCODE			0x4432

/* Formats:
 * 	RGB565		= 0x4432, 
	BGR565		= 0xC432, 
	XRGB1555	= 0x4342,
	XBGR1555	= 0xC342,
	XRGB4444	= 0x4211,
	XBGR4444	= 0xC211,
	XRGB8332	= 0x4120,
	XBGR8332	= 0xC120,
	ARGB1555	= 0x3342,
	ABGR1555	= 0xB342,
	ARGB4444	= 0x2211,
	ABGR4444	= 0xA211,
	ARGB8332	= 0x1120,
	ABGR8332	= 0x9120,
	RGB888		= 0x4653,
	BGR888		= 0xC653,
	XRGB8888	= 0x4653, 
	XBGR8888	= 0xC653, 
	ARGB8888	= 0x0653,
	ABGR8888	= 0x8653,
	PTRGB565	= 0x443A
 * */


/* Get MLC FB address and size from mlc_fb=ADDR, SIZE kernel cmd line arg */
static u32 mlc_fb_addr;
static u32 mlc_fb_size;

/* fixed framebuffer settings */
static struct fb_fix_screeninfo lf1000fb_fix __initdata = {
	.id		= "lf1000-fb",
	.smem_start	= 0,	// filled in from Linux cmd line
	.smem_len	= 0,	// filled in from Linux cmd line
	.type		= FB_TYPE_PACKED_PIXELS,
	.visual		= VISUALTYPE, 
	.type_aux	= 0,
	.xpanstep	= 0,
	.ypanstep	= 0,
	.ywrapstep	= 0,
	.line_length	= X_RESOLUTION*BYTESPP, //was X_RESOLUTION for 8 bit ? X_RESOLUTION*4,
	.accel		= FB_ACCEL_NONE,
};

static int __init lf1000fb_fb_setup(char *str)
{
	char *s;
	mlc_fb_addr = simple_strtol(str, &s, 0);
	lf1000fb_fix.smem_start=mlc_fb_addr;
	if (*s == ',') {
		mlc_fb_size = simple_strtol(s+1, &s, 0);
		lf1000fb_fix.smem_len=mlc_fb_size;
	}
	return 1;
}

__setup("mlc_fb=", lf1000fb_fb_setup);

/*
 * driver private data
 */

struct lf1000fb_info {
	struct fb_info			fb;
	struct device			*dev;

	void				*fbmem;

	int pseudo_pal[16];
	int palette_buf[256];
};


/*
 * Configure the MLC - we dont know what mode we are inheriting... 
 *
*/

static void *memregs;
static void set_mlc(struct platform_device *pdev)
{
	u32 reg;
	u32 hstride;
	u32 vstride;
	u32 bgcolor;
	u32 dirtyflag;
	u32 enable;
	u16 currentformat;
	u16 newformat;

	struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		printk(KERN_INFO "lf1000fb: **************can't get resource\n");
	}
	
	if (!request_mem_region(res->start, (res->end - res->start+1), "lf1000-fb")) {
		printk(KERN_INFO  "lf1000fb: *************can't request memory\n");
	}
	
	memregs = ioremap_nocache(res->start, (res->end - res->start+1));
	if(!memregs) {
		printk(KERN_INFO "lf1000fb: **************can't remap memory\n");
	}
	
	enable = ioread32(memregs+MLCCONTROL0);						/*Read the register*/
	iowrite32(enable | (1<<LAYERENB), memregs+MLCCONTROL0);		/*Enable Layer*/
	enable = ioread32(memregs+MLCCONTROL1);						/*Read the register*/
	iowrite32(enable | (1<<LAYERENB), memregs+MLCCONTROL1);		/*Enable Layer*/
	
	printk(KERN_INFO "lf1000fb: Current MLC0 Mode: 0x%X\n", (ioread32(memregs+MLCCONTROL0)>>FORMAT) & 0xFFFF);
	printk(KERN_INFO "lf1000fb: Current MLC1 Mode: 0x%X\n", (ioread32(memregs+MLCCONTROL1)>>FORMAT) & 0xFFFF);

	reg=ioread32(memregs+MLCCONTROL0) & ~(0xFFFF<<FORMAT);
	iowrite32(reg | (FORMATCODE<<FORMAT), memregs+MLCCONTROL0); /*Write the format */
	reg=ioread32(memregs+MLCCONTROL1) & ~(0xFFFF<<FORMAT);
	iowrite32(reg | (FORMATCODE<<FORMAT), memregs+MLCCONTROL1); /*Write the format */
	
	printk(KERN_INFO "lf1000fb: New MLC0 Mode: 0x%X\n", (ioread32(memregs+MLCCONTROL0)>>FORMAT) & 0xFFFF);
	printk(KERN_INFO "lf1000fb: New MLC1 Mode: 0x%X\n", (ioread32(memregs+MLCCONTROL1)>>FORMAT) & 0xFFFF);

		
	hstride = BYTESPP;	
	hstride &= 0x7FFFFFFF;						
	iowrite32(hstride, memregs + MLCHSTRIDE0);
	iowrite32(hstride, memregs + MLCHSTRIDE1);

	printk(KERN_INFO "lf1000fb: New MLC0 HStride: %d\n", (ioread32(memregs + MLCHSTRIDE0)));
	printk(KERN_INFO "lf1000fb: New MLC1 HStride: %d\n", (ioread32(memregs + MLCHSTRIDE1)));

	
	vstride = hstride*X_RESOLUTION;
	iowrite32(vstride, memregs + MLCVSTRIDE0);
	iowrite32(vstride, memregs + MLCVSTRIDE1);

	printk(KERN_INFO "lf1000fb: New VStride: %d\n", (ioread32(memregs + MLCVSTRIDE0)));
	printk(KERN_INFO "lf1000fb: New VStride: %d\n", (ioread32(memregs + MLCVSTRIDE1)));


	dirtyflag = ioread32(memregs+MLCCONTROL0);						/*Read the register*/
	iowrite32(dirtyflag | (1<<DIRTYFLAG), memregs+MLCCONTROL0);		/*Set dirty*/
	dirtyflag = ioread32(memregs+MLCCONTROL1);						/*Read the register*/
	iowrite32(dirtyflag | (1<<DIRTYFLAG), memregs+MLCCONTROL1);		/*Set dirty*/

}

/*
 * framebuffer interface 
 */

static void schedule_palette_update(struct lf1000fb_info *fbi,
		unsigned int regno, unsigned int val)
{
	printk(KERN_INFO "lf1000fb_schedule_palette_update\n");
	unsigned long flags;

	local_irq_save(flags);

	fbi->palette_buf[regno] = val;

	local_irq_restore(flags);
}

static inline unsigned int chan_to_field(unsigned int chan, struct fb_bitfield *bf)
{
	chan &= 0xffff;
	chan >>= 16 - bf->length;
	return chan << bf->offset;
}

static int lf1000fb_setcolreg(unsigned regno,
		unsigned red, unsigned green, unsigned blue,
		unsigned transp, struct fb_info *info)
{
	//printk(KERN_INFO "lf1000fb: setcolreg\n");
	struct lf1000fb_info *fbi = info->par;
	unsigned int val;

	switch (info->fix.visual) {
		case FB_VISUAL_TRUECOLOR:
			//printk(KERN_INFO "lf1000fb_setcolreg:FB_VISUAL_TRUECOLOR\n");
			/* true-colour, use pseudo-palette */

			if (regno < 16) {
				u32 *pal = fbi->pseudo_pal;

				val  = chan_to_field(red,   &info->var.red);
				val |= chan_to_field(green, &info->var.green);
				val |= chan_to_field(blue,  &info->var.blue);

				pal[regno] = val;
			}
			break;

		case FB_VISUAL_PSEUDOCOLOR:
			//printk(KERN_INFO "lf1000fb_setcolreg:FB_VISUAL_PSEUDOCOLOR\n");
			if (regno < 256) {
				/* currently assume RGB 5-6-5 mode */

				//val  = ((red   >>  0) & 0xf800);
				//val |= ((green >>  5) & 0x07e0);
				//val |= ((blue  >> 11) & 0x001f);
				val = ((red>>8) & 0xF800);
				val |= ((green>>5) & 0x07E0);
				val |= ((blue>>11) & 0x001F);
				schedule_palette_update(fbi, regno, val);
			}

			break;

		default:
			return 1;   /* unknown type */
	}

	return 0;
}



//normally in drivers/lf1000/mlc_hal.h

#define MLCCONTROLT             0x00
#define MLCSCREENSIZE		    0x04
#define MLCADDRESS0    			0x38
#define MLCCONTROL1				0x58
#define MLCHSTRIDE1				0x5C
#define MLCVSTRIDE1				0x60
#define MLCADDRESS1				0x6C
#define MLCCONTROL2				0x7C
#define MLCADDRESS2     		0x8C    /* MLCADDRESS3 in databook */
#define MLCVSTRIDE2				0x80	/* MLCVSTRIDE2 in databook */
#define MLCSTRIDECB   		    0x98
#define MLCSTRIDECR     		0x9C

#define MLCTOPBOTTOM0			0x10
#define MLCLEFTRIGHT0			0x0C
#define MLCLEFTOP0				0x0C//????
#define MLCRIGHTBOTTOM0			0x10//????
#define MLCTOPBOTTOM1			0x44
#define MLCLEFTRIGHT1			0x40
#define MLCLEFTOP1				0x2C //????
#define MLCRIGHTBOTTOM1			0x30 //????

#define MLCTOPBOTTOM2			0x78
#define MLCLEFTRIGHT2			0x74
#define MLCLEFTOP2				0x4C //????
#define MLCRIGHTBOTTOM2			0x50 //?????


#define MLCTPCOLOR2				0x84 //databook error says tpcolor3
#define MLCTPCOLOR1				0x64
#define MLCTPCOLOR0				0x30

#define MLCINVCOLOR0			0x24
#define MLCINVCOLOR1			0x44
#define MLCINVCOLOR2			0x64

#define MLCHSCALE				0x9C
#define MLCVSCALE				0xA0


#define MLCLEFTRIGHT0_0			0x14
#define MLCLEFTRIGHT1_0			0x48

#define MLCTOPBOTTOM0_0			0x18
#define MLCTOPBOTTOM1_0			0x4C

#define MLCADDRESSCB			0x8C
#define MLCADDRESSCR			0x90

/* MLC RGB Layer n Control Register (MLCCONTROLn) */
#define GRP3DENB                8       /* set layer as output of 3D core */
#define LAYERENB                5       /* enable the layer */
#define PALETTEPWD              15      /* layer n palette table on/off */
#define PALETTESLD              14      /* layer n palette table sleep mode */
#define MLC_NUM_LAYERS			3
#define MLC_VIDEO_LAYER		(MLC_NUM_LAYERS-1)
#define LOCKSIZE		12	/* memory read size */
#define BLENDENB		2
#define INVENB			1
#define TPENB			0

/* MLC TOP CONTROL REGISTER (MLCCONTROLT) */
#define DITTYFLAG               3
#define PIXELBUFFER_SLD         10      /* pixel buffer sleep mode */
#define PIXELBUFFER_PWD         11      /* pixel buffer power on/off */
#define MLCENB                  1
#define PRIORITY		8

/* MLC SCREEN SIZE REGISTER (MLCSCREENSIZE) */
#define SCREENHEIGHT		16
#define SCREENWIDTH		0

/* MLC RGB Layer n Left Right Register (MLCLEFTRIGHTn) */
#define LEFT			16
#define RIGHT			0

/* MLC RGB Layer n Top Bottom Register (MLCTOPBOTTOMn) */
#define TOP			16
#define BOTTOM			0

/* MLC RGB LAYER n TRANSPARENCY COLOR REGISTER (MLCTPCOLORn) */
#define ALPHA			28
#define TPCOLOR			0

/* MLC RGB LAYER n INVERSION COLOR REGISTER (MLCINVCOLORn) */
#define INVCOLOR		0

/* MLC Video layer Horizontal Scale (MLCHSCALE) Register */

#define HFILTERENB		28 /* bilinear filter enable */
#define HSCALE			0  /* horizonal scale ratio */

/* MLC Video layer Vertical Scale (MLCVSCALE) Register */

#define VFILTERENB		28 /* bilinear filter enable */
#define VSCALE			0  /* vertical scale ratio */


/* MLC RGB Layer n Invalid Area 0 left right register (MLCLEFTRIGHT0_0) */
#define INVALIDENB		28
#define INVALIDLEFT		16
#define INVALIDRIGHT		0

/* MLC RGB Layer n Invalid Area 0 Top Bottom Register */
#define INVALIDTOP		16
#define INVALIDBOTTOM		0


//enum {
//	VID_PRIORITY_FIRST	= 0,   /* video  > Cursor > Window > 3D */
//	VID_PRIORITY_SECOND	= 1,   /* Cursor > video  > Window > 3D */
//	VID_PRIORITY_THIRD	= 2,   /* Cursor > Window > video  > 3D */
//	VID_PRIORITY_INVALID	= 3,
//};

//enum RGBFMT
//{
	//VID_RGBFMT_R5G6B5	= 0x4432,   /* 16bpp { R5, G6, B5 }. */
	//VID_RGBFMT_B5G6R5	= 0xC432,   /* 16bpp { B5, G6, R5 }. */

	//VID_RGBFMT_X1R5G5B5	= 0x4342,   /* 16bpp { X1, R5, G5, B5 }. */
	//VID_RGBFMT_X1B5G5R5	= 0xC342,   /* 16bpp { X1, B5, G5, R5 }. */
	//VID_RGBFMT_X4R4G4B4	= 0x4211,   /* 16bpp { X4, R4, G4, B4 }. */
	//VID_RGBFMT_X4B4G4R4	= 0xC211,   /* 16bpp { X4, B4, G4, R4 }. */
	//VID_RGBFMT_X8R3G3B2	= 0x4120,   /* 16bpp { X8, R3, G3, B2 }. */
	//VID_RGBFMT_X8B3G3R2	= 0xC120,   /* 16bpp { X8, B3, G3, R2 }. */

	//VID_RGBFMT_A1R5G5B5	= 0x3342,   /* 16bpp { A1, R5, G5, B5 }. */
	//VID_RGBFMT_A1B5G5R5	= 0xB342,   /* 16bpp { A1, B5, G5, R5 }. */
	//VID_RGBFMT_A4R4G4B4	= 0x2211,   /* 16bpp { A4, R4, G4, B4 }. */
	//VID_RGBFMT_A4B4G4R4	= 0xA211,   /* 16bpp { A4, B4, G4, R4 }. */
	//VID_RGBFMT_A8R3G3B2	= 0x1120,   /* 16bpp { A8, R3, G3, B2 }. */
	//VID_RGBFMT_A8B3G3R2	= 0x9120,   /* 16bpp { A8, B3, G3, R2 }. */

	//VID_RGBFMT_G8R8_G8B8	= 0x4ED3,   /* 16bpp { G8, R8, G8, B8 }. */
	//VID_RGBFMT_R8G8_B8G8	= 0x4F84,   /* 16bpp { R8, G8, B8, G8 }. */
	//VID_RGBFMT_G8B8_G8R8	= 0xCED3,   /* 16bpp { G8, B8, G8, R8 }. */
	//VID_RGBFMT_B8G8_R8G8	= 0xCF84,   /* 16bpp { B8, G8, R8, G8 }. */
	//VID_RGBFMT_X8L8		= 0x4003,   /* 16bpp { X8, L8 }. */
	//VID_RGBFMT_A8L8		= 0x1003,   /* 16bpp { A8, L8 }. */
	//VID_RGBFMT_L16		= 0x4554,   /* 16bpp { L16 }. */

	//VID_RGBFMT_R8G8B8	= 0x4653,   /* 24bpp { R8, G8, B8 }. */
	//VID_RGBFMT_B8G8R8	= 0xC653,   /* 24bpp { B8, G8, R8 }. */

	//VID_RGBFMT_X8R8G8B8	= 0x4653,   /* 32bpp { X8, R8, G8, B8 }. */
	//VID_RGBFMT_X8B8G8R8	= 0xC653,   /* 32bpp { X8, B8, G8, R8 }. */
	//VID_RGBFMT_A8R8G8B8	= 0x0653,   /* 32bpp { A8, R8, G8, B8 }. */
	//VID_RGBFMT_A8B8G8R8	= 0x8653,   /* 32bpp { A8, B8, G8, R8 }. */
//};

/* bit masking */
#define BIT_SET(v,b)	(v |= (1<<(b)))
#define BIT_CLR(v,b)	(v &= ~(1<<(b)))
#define IS_SET(v,b)	(v & (1<<(b)))
#define IS_CLR(v,b)	!(v & (1<<(b)))
#define BIT_MASK_ONES(b) ((1<<(b))-1)


//#define FBIO_MAGIC			'D' //used in the pollux driver. not sure why
//#define MLC_IOCTBACKGND		_IOW(FBIO_MAGIC, 100, unsigned int)
//#define MLC_IOCTLAYEREN		_IOW(FBIO_MAGIC, 101, unsigned int)
//#define MLC_IOCTADDRESS		_IOW(FBIO_MAGIC, 102, unsigned int)
//#define MLC_IOCTHSTRIDE		_IOW(FBIO_MAGIC, 103, unsigned int)
//#define MLC_IOCTVSTRIDE		_IOW(FBIO_MAGIC, 104, unsigned int)
//#define MLC_IOCT3DENB		_IOW(FBIO_MAGIC, 105, unsigned int)
//#define MLC_IOCTDIRTY		_IOW(FBIO_MAGIC, 106, unsigned int)
//#define MLC_IOCQDIRTY		_IOR(FBIO_MAGIC, 107, unsigned int)
//#define MLC_IOCQHSTRIDE		_IOR(FBIO_MAGIC, 108, unsigned int)

//ioctl.h
struct position_cmd {
	unsigned int top;
   	unsigned int left;
	unsigned int right;
	unsigned int bottom;
};

struct screensize_cmd {
	unsigned int width;
	unsigned int height;
};

struct overlaysize_cmd {
	unsigned int srcwidth;
	unsigned int srcheight;
	unsigned int dstwidth;
	unsigned int dstheight;
};

union mlc_cmd {
	struct position_cmd position;
	struct screensize_cmd screensize;
	struct overlaysize_cmd overlaysize;
};

//normally in arch/arm/lf1000/mach/mlc.h
int mlc_GetAddressCb(u8 layer, int *addr);
int mlc_GetAddressCr(u8 layer, int *addr);


//normally in include/linux/lf1000
#define MLC_IOC_MAGIC   'm'
#define MLC_IOCTENABLE		_IO(MLC_IOC_MAGIC,  0)
#define MLC_IOCTBACKGND		_IO(MLC_IOC_MAGIC,  1)
#define MLC_IOCQBACKGND		_IO(MLC_IOC_MAGIC,  2)
#define MLC_IOCTPRIORITY	_IO(MLC_IOC_MAGIC,  3)
#define MLC_IOCQPRIORITY	_IO(MLC_IOC_MAGIC,  4)
#define MLC_IOCTTOPDIRTY	_IO(MLC_IOC_MAGIC,  5)
#define MLC_IOCSSCREENSIZE	_IOW(MLC_IOC_MAGIC, 6, struct screensize_cmd *)
#define MLC_IOCGSCREENSIZE	_IOR(MLC_IOC_MAGIC, 7, struct screensize_cmd *)



#define MLC_IOCTLAYEREN		_IO(MLC_IOC_MAGIC,  8)
#define MLC_IOCTADDRESS		_IO(MLC_IOC_MAGIC,  9)
#define MLC_IOCQADDRESS		_IO(MLC_IOC_MAGIC,  25)
#define MLC_IOCQFBSIZE		_IO(MLC_IOC_MAGIC,  29)


#define MLC_IOCTHSTRIDE		_IO(MLC_IOC_MAGIC,  10)
#define MLC_IOCQHSTRIDE		_IO(MLC_IOC_MAGIC,  26)
#define MLC_IOCTVSTRIDE		_IO(MLC_IOC_MAGIC,  11)
#define MLC_IOCQVSTRIDE		_IO(MLC_IOC_MAGIC,  27)

	
#define MLC_IOCTLOCKSIZE	_IO(MLC_IOC_MAGIC,  12)
#define MLC_IOCSPOSITION	_IOW(MLC_IOC_MAGIC, 13, struct position_cmd *)
#define MLC_IOCGPOSITION	_IOR(MLC_IOC_MAGIC, 14, struct position_cmd *)
#define MLC_IOCTFORMAT		_IO(MLC_IOC_MAGIC,  15)
#define MLC_IOCQFORMAT		_IO(MLC_IOC_MAGIC,  16)

#define MLC_IOCTDIRTY		_IO(MLC_IOC_MAGIC,  17)
#define MLC_IOCQDIRTY		_IO(MLC_IOC_MAGIC,  32)
#define MLC_IOCT3DENB		_IO(MLC_IOC_MAGIC,  18)
#define MLC_IOCQ3DENB		_IO(MLC_IOC_MAGIC,  33)

#define MLC_IOCTALPHA		_IO(MLC_IOC_MAGIC,  19)
#define MLC_IOCQALPHA		_IO(MLC_IOC_MAGIC,  28)
#define MLC_IOCTTPCOLOR		_IO(MLC_IOC_MAGIC,  20)
#define MLC_IOCQTPCOLOR		_IO(MLC_IOC_MAGIC,  40) //PATCH
#define MLC_IOCTBLEND		_IO(MLC_IOC_MAGIC,  21)
#define MLC_IOCQBLEND		_IO(MLC_IOC_MAGIC,  41) //PATCH
#define MLC_IOCTTRANSP		_IO(MLC_IOC_MAGIC,  22)
#define MLC_IOCQTRANSP		_IO(MLC_IOC_MAGIC,  42) //PATCH

#define MLC_IOCTINVERT		_IO(MLC_IOC_MAGIC,  23)
#define MLC_IOCQINVERT		_IO(MLC_IOC_MAGIC,  43) //PATCH

#define MLC_IOCTINVCOLOR	_IO(MLC_IOC_MAGIC,  24)
#define MLC_IOCQINVCOLOR	_IO(MLC_IOC_MAGIC,  44)  //PATCH

#define MLC_IOCSOVERLAYSIZE	_IOW(MLC_IOC_MAGIC, 30, struct overlaysize_cmd *)
#define MLC_IOCGOVERLAYSIZE	_IOR(MLC_IOC_MAGIC, 31, struct overlaysize_cmd *)
#define MLC_IOCTINVISIBLE	_IO(MLC_IOC_MAGIC,  34)
#define MLC_IOCQINVISIBLE	_IO(MLC_IOC_MAGIC,  35)

#define MLC_IOCSINVISIBLEAREA	_IOW(MLC_IOC_MAGIC, 36, struct position_cmd *)
#define MLC_IOCGINVISIBLEAREA	_IOR(MLC_IOC_MAGIC, 37, struct position_cmd *)

#define MLC_IOCTADDRESSCB	_IO(MLC_IOC_MAGIC,  38)
#define MLC_IOCQADDRESSCB	_IO(MLC_IOC_MAGIC,  45) //PATCH

#define MLC_IOCTADDRESSCR	_IO(MLC_IOC_MAGIC,  39)
#define MLC_IOCQADDRESSCR	_IO(MLC_IOC_MAGIC,  46) //PATCH


int have_tvout(void)
{
#ifdef DUAL_DISPLAY		
	return(1);
#else
	return(0);
#endif
}

//int mlc_layer_ioctl(struct inode *inode, struct file *filp, unsigned int cmd,unsigned long arg)
//static int pollux_ioctl(struct fb_info *info, unsigned int cmd, unsigned long arg)

static int lf1000fb_ioctl(struct fb_info *info, unsigned int cmd, unsigned long arg)
{

	int result = 0;
	void __user *argp = (void __user *)arg;
	union mlc_cmd c;
	//int size = 0;
	//void *pdata = NULL;
	//struct lf1000fb_info *fbi;
	/* Check if IOCTL code is valid */
	if(_IOC_TYPE(cmd) != MLC_IOC_MAGIC)
		return -EINVAL;
	
	/* Allocate argument buffer for READ/WRITE ioctl commands */
	//if(_IOC_DIR(cmd) != _IOC_NONE)
	//{		
	//	size = _IOC_SIZE(cmd);
	//	pdata = vmalloc(size);
	//}
	
	/* Get base address of the device driver specific information */
	//fbi = info->par;


	int layerID = 0; //hard coded, for now

	switch(cmd) {
		
		case MLC_IOCTENABLE:
		mlc_SetMLCEnable(arg);
		if (have_tvout()) {
			memregs += 0x400;
			mlc_SetMLCEnable(arg);
			memregs -= 0x400;
		}
		break;
		
		case MLC_IOCQBACKGND:
		result = mlc_GetBackground();
		break;
		
		case MLC_IOCTBACKGND:
		mlc_SetBackground(arg);
		if (have_tvout()) {
			memregs += 0x400;
			mlc_SetBackground(arg);
			memregs -= 0x400;
		}
		break;
		
		case MLC_IOCQPRIORITY:
		result = mlc_GetLayerPriority();
		break;
		
		case MLC_IOCTPRIORITY:
		result = mlc_SetLayerPriority(arg);
		if (have_tvout()) {
			memregs += 0x400;
			result = mlc_SetLayerPriority(arg);
			memregs -= 0x400;
		}
		break;

		case MLC_IOCTTOPDIRTY:
		mlc_SetTopDirtyFlag();
		if (have_tvout()) {
			memregs += 0x400;
			mlc_SetTopDirtyFlag();
			memregs -= 0x400;
		}
		break;
		
		case MLC_IOCSSCREENSIZE:
		if(!(_IOC_DIR(cmd) & _IOC_WRITE))
			return -EFAULT;
		if(copy_from_user((void *)&c, argp, sizeof(struct screensize_cmd)))
			return -EFAULT;
		result = mlc_SetScreenSize(c.screensize.width,
					   c.screensize.height);
		if (have_tvout()) {
			memregs += 0x400;
			result = mlc_SetScreenSize(c.screensize.width,
						   c.screensize.height);
			memregs -= 0x400;
		}
		break;
		
		case MLC_IOCGSCREENSIZE:
		if(!(_IOC_DIR(cmd) & _IOC_READ))
			return -EFAULT;
		mlc_GetScreenSize((struct mlc_screen_size *)&c);
		if(copy_to_user(argp, (void *)&c, sizeof(struct screensize_cmd)))
			return -EFAULT;
		break;
		
		case MLC_IOCTLAYEREN:
		result = mlc_SetLayerEnable(layerID, arg);
		if (have_tvout()) {
			memregs+= 0x400;
			result = mlc_SetLayerEnable(layerID, arg);
			memregs -= 0x400;
		}
		break;

		case MLC_IOCTADDRESS:
		result = mlc_SetAddress(layerID, arg);
		if (have_tvout()) {
			memregs += 0x400;
			result = mlc_SetAddress(layerID, arg);
			memregs -= 0x400;
		}
		break;

		case MLC_IOCTHSTRIDE:
		result = mlc_SetHStride(layerID, arg);
		if (have_tvout()) {
			memregs+= 0x400;
			result = mlc_SetHStride(layerID, arg);
			memregs -= 0x400;
		}
		break;

		case MLC_IOCTVSTRIDE:
		result = mlc_SetVStride(layerID, arg);
		if (have_tvout()) {
			memregs += 0x400;
			result = mlc_SetVStride(layerID, arg);
			memregs -= 0x400;
		}
		break;
		
		case MLC_IOCQVSTRIDE:
		result = mlc_GetVStride(layerID);
		break;
		
		case MLC_IOCTLOCKSIZE:
		result = mlc_SetLockSize(layerID, arg);
		if (have_tvout()) {
			memregs += 0x400;
			result = mlc_SetLockSize(layerID, arg);
			memregs -= 0x400;
		}
		break;


		case MLC_IOCSPOSITION:
		if(!(_IOC_DIR(cmd) & _IOC_WRITE))
			return -EFAULT;
		if(copy_from_user((void *)&c, argp, 
				  sizeof(struct position_cmd)))
			return -EFAULT;
		result = mlc_SetPosition(layerID,
					 c.position.top,
					 c.position.left,
					 c.position.right,
					 c.position.bottom);
		if (have_tvout()) {
			memregs += 0x400;
			result = mlc_SetPosition(layerID,
						 c.position.top,
						 c.position.left,
						 c.position.right,
						 c.position.bottom);
			memregs -= 0x400;
		}
		break;
		
		case MLC_IOCGPOSITION:
		if(!(_IOC_DIR(cmd) & _IOC_READ))
			return -EFAULT;
		result = mlc_GetPosition(layerID, 
					 (struct mlc_layer_position *)&c);
		if(result < 0)
			return result;
		if(copy_to_user(argp, (void *)&c, sizeof(struct position_cmd)))
			return -EFAULT;
		break;


		case MLC_IOCTFORMAT:
		result = mlc_SetFormat(layerID, arg);
		if (have_tvout()) {
			memregs += 0x400;
			result = mlc_SetFormat(layerID, arg);
			memregs -= 0x400;
		}
		break;
		
		case MLC_IOCQFORMAT:
		if(mlc_GetFormat(layerID, &result) < 0)
			return -EFAULT;
		break;

		case MLC_IOCT3DENB:
		result = mlc_Set3DEnable(layerID, arg);
		if (have_tvout()) {
			memregs += 0x400;
			result = mlc_Set3DEnable(layerID, arg);
			memregs -= 0x400;
		}
		break;
		
		case MLC_IOCQ3DENB:
		if(mlc_Get3DEnable(layerID, &result) < 0)
			return -EFAULT;
		break;
		//result = mlc_Get3DEnable(layerID);
		


		case MLC_IOCTALPHA:
		result = mlc_SetTransparencyAlpha(layerID, arg);
		if (have_tvout()) {
			memregs += 0x400;
			result = mlc_SetTransparencyAlpha(layerID, arg);
			memregs -= 0x400;
		}
		break;
		
		case MLC_IOCQALPHA:
		result = mlc_GetTransparencyAlpha(layerID);
		break;
		
		case MLC_IOCTTPCOLOR:
		result = mlc_SetTransparencyColor(layerID, arg);
		if (have_tvout()) {
			memregs += 0x400;
			result = mlc_SetTransparencyColor(layerID, arg);
			memregs -= 0x400;
		}
		break;
		
		
		case MLC_IOCQTPCOLOR:
		if(mlc_GetTransparencyColor(layerID, &result) < 0)
			return -EFAULT;
		break;
		
		
		case MLC_IOCTBLEND:
		result = mlc_SetBlendEnable(layerID, arg);
		if (have_tvout()) {
			memregs += 0x400;
			result = mlc_SetBlendEnable(layerID, arg);
			memregs -= 0x400;
		}
		break;
		
		case MLC_IOCQBLEND:
		if(mlc_GetBlendEnable(layerID, &result) < 0);
			return -EFAULT;
		break;
		
		case MLC_IOCTTRANSP:
		result = mlc_SetTransparencyEnable(layerID, arg);
		if (have_tvout()) {
			memregs += 0x400;
			result = mlc_SetTransparencyEnable(layerID, arg);
			memregs -= 0x400;
		}
		break;
		
		case MLC_IOCQTRANSP:
		if(mlc_GetTransparencyEnable(layerID, &result) < 0);
			return -EFAULT;
		break;
		
		case MLC_IOCTINVERT:
		result = mlc_SetInvertEnable(layerID, arg);
		if (have_tvout()) {
			memregs += 0x400;
			result = mlc_SetInvertEnable(layerID, arg);
			memregs -= 0x400;
		}
		break;
		
		case MLC_IOCQINVERT:
		if(mlc_GetInvertEnable(layerID, &result) < 0);
			return -EFAULT;
		break;
		
		case MLC_IOCTINVCOLOR:
		result = mlc_SetInvertColor(layerID, arg);
		if (have_tvout()) {
			memregs += 0x400;
			result = mlc_SetInvertColor(layerID, arg);
			memregs -= 0x400;
		}
		
		case MLC_IOCQINVCOLOR:
		if(mlc_GetInvertColor(layerID, &result) < 0);
			return -EFAULT;
		break;
		
		case MLC_IOCSOVERLAYSIZE:
		if(!(_IOC_DIR(cmd) & _IOC_WRITE))
			return -EFAULT;
		if(copy_from_user((void *)&c, argp, 
				  sizeof(struct overlaysize_cmd)))
			return -EFAULT;
		result = mlc_SetOverlaySize(layerID,
					    c.overlaysize.srcwidth,
					    c.overlaysize.srcheight,
					    c.overlaysize.dstwidth,
					    c.overlaysize.dstheight);
		if (have_tvout()) {
			memregs += 0x400;
			result = mlc_SetOverlaySize(layerID,
						    c.overlaysize.srcwidth,
						    c.overlaysize.srcheight,
						    c.overlaysize.dstwidth,
						    c.overlaysize.dstheight);
			memregs -= 0x400;
		}
		break;
		
		case MLC_IOCGOVERLAYSIZE:
		if(!(_IOC_DIR(cmd) & _IOC_READ))
			return -EFAULT;
		result = mlc_GetOverlaySize(layerID, 
					    (struct mlc_overlay_size *)&c);
		if(result < 0)
			return result;
		if(copy_to_user(argp, (void *)&c, 
				sizeof(struct overlaysize_cmd)))
			return -EFAULT;
		break;
		
		
		case MLC_IOCTINVISIBLE:
		result = mlc_SetLayerInvisibleAreaEnable(layerID, arg);
		break;
		
		case MLC_IOCQINVISIBLE:
		result = mlc_GetLayerInvisibleAreaEnable(layerID);
		break;

		case MLC_IOCSINVISIBLEAREA:
		if(!(_IOC_DIR(cmd) & _IOC_WRITE))
			return -EFAULT;
		if(copy_from_user((void *)&c, argp, 
				  sizeof(struct position_cmd)))
			return -EFAULT;
		result = mlc_SetLayerInvisibleArea(layerID,
						   c.position.top,
					  	   c.position.left,
					  	   c.position.right,
					  	   c.position.bottom);
		break;
		
		case MLC_IOCGINVISIBLEAREA:
		if(!(_IOC_DIR(cmd) & _IOC_READ))
			return -EFAULT;
		result = mlc_GetLayerInvisibleArea(layerID, 
					(struct mlc_layer_position *)&c);
		if(result < 0)
			return result;
		if(copy_to_user(argp, (void *)&c, sizeof(struct position_cmd)))
			return -EFAULT;
		break;
		
		
		case MLC_IOCTADDRESSCB:
		result = mlc_SetAddressCb(layerID, arg);
		if (have_tvout()) {
			memregs += 0x400;
			result = mlc_SetAddressCb(layerID, arg);
			memregs -= 0x400;
		}
		break;
		

		
		case MLC_IOCTADDRESSCR:
		result = mlc_SetAddressCr(layerID, arg);
		if (have_tvout()) {
			memregs += 0x400;
			result = mlc_SetAddressCr(layerID, arg);
			memregs -= 0x400;
		}
		break;
		

		case MLC_IOCTDIRTY:
		result = mlc_SetDirtyFlag(layerID);
		if (have_tvout()) {
			memregs += 0x400;
			result = mlc_SetDirtyFlag(layerID);
			memregs -= 0x400;
		}
		break;

		case MLC_IOCQDIRTY:
		/* query 2nd MLC for proper sync on TV + LCD out */
		if (have_tvout()) {
			memregs += 0x400;
			result = mlc_GetDirtyFlag(layerID);
			memregs -= 0x400;
		} else {
			result = mlc_GetDirtyFlag(layerID);
		}
		break;
		

		case MLC_IOCQHSTRIDE:
		result = mlc_GetHStride(layerID);
		break;
		
		case MLC_IOCQADDRESS:
		if(mlc_GetAddress(layerID, &result) < 0)
		return -EFAULT;
		break;

		case MLC_IOCQADDRESSCR:
		if(mlc_GetAddressCr(layerID, &result) < 0)
		return -EFAULT;
		break;
		
		case MLC_IOCQADDRESSCB:
		if(mlc_GetAddressCb(layerID, &result) < 0)
		return -EFAULT;
		break;
				

		case MLC_IOCQFBSIZE:
		if (layerID > MLC_NUM_LAYERS)
			return -EFAULT;
		result = mlc_fb_size;
		break;
		


		default:
		return -ENOTTY;
	}

	return result;
}




void mlc_SetMLCEnable(u8 en)
{
	u32 tmp = ioread32(memregs+MLCCONTROLT);

	BIT_CLR(tmp,DITTYFLAG);

	if(en) {
		BIT_SET(tmp,PIXELBUFFER_PWD); 	/* power up */
		iowrite32(tmp, memregs+MLCCONTROLT);
		BIT_SET(tmp,PIXELBUFFER_SLD); 	/* disable sleep */
		iowrite32(tmp, memregs+MLCCONTROLT);
	}
	else {
		BIT_CLR(tmp,MLCENB);		/* disable */
		BIT_SET(tmp,DITTYFLAG);
		iowrite32(tmp, memregs+MLCCONTROLT);
		do { /* wait for MLC to turn off */
			tmp = ioread32(memregs+MLCCONTROLT);
		} while(IS_SET(tmp,DITTYFLAG));
		BIT_CLR(tmp,PIXELBUFFER_SLD);	/* enable sleep */
		iowrite32(tmp, memregs+MLCCONTROLT);
		BIT_CLR(tmp,PIXELBUFFER_PWD);	/* power down */
	}

	iowrite32(tmp,memregs+MLCCONTROLT);
}



static void *SelectLayerControl(u8 layer)
{
	void *ctl = memregs;

	if(!memregs) {
		return 0;
	}
	switch(layer) {
		case 0:
		ctl += MLCCONTROL0;
		break;
		case 1:
		ctl += MLCCONTROL1;
		break;
		case 2:
		ctl += MLCCONTROL2;
		break;
		default:
		return 0;
	}

	return ctl;
}

int mlc_SetLayerEnable(u8 layer, u8 en)
{
	void *reg;
	u32 tmp;

	if(layer > MLC_NUM_LAYERS)
		return -EINVAL;

	reg = SelectLayerControl(layer);
	tmp = ioread32(reg);

	BIT_SET(tmp,PALETTEPWD); /* power up */
	iowrite32(tmp,reg);
	en ? BIT_SET(tmp,PALETTESLD) : BIT_CLR(tmp,PALETTESLD); /* disable sleep mode */
	iowrite32(tmp,reg);
	en ? BIT_SET(tmp,LAYERENB) : BIT_CLR(tmp,LAYERENB);

	iowrite32(tmp,reg);
	return 0;
}

int mlc_GetAddress(u8 layer, int *addr) /* FIXME */
{
	void *reg = NULL;

	if(layer > MLC_NUM_LAYERS)
		return -EINVAL;
	
	switch(layer) {
		case 0:
		reg = memregs+MLCADDRESS0;
		break;
		case 1:
		reg = memregs+MLCADDRESS1;
		break;
		case 2:
		reg = memregs+MLCADDRESS2; /* note: weird datasheet naming - says MLCADDRESS3 */
		break;
	}

	*addr = ioread32(reg);
	return 0;
}


int mlc_GetAddressCb(u8 layer, int *addr)
{
	void *reg = NULL;
	if(layer > MLC_NUM_LAYERS || layer != MLC_VIDEO_LAYER)
		return -EINVAL;

	reg = memregs+MLCADDRESSCB;
	*addr = ioread32(reg);
	return 0;
}

int mlc_GetAddressCr(u8 layer, int *addr)
{
	void *reg = NULL;
	
	if(layer > MLC_NUM_LAYERS || layer != MLC_VIDEO_LAYER)
		return -EINVAL;

	reg = memregs+MLCADDRESSCR;
	*addr = ioread32(reg);
	return 0;
}


int mlc_SetAddress(u8 layer, u32 addr)
{
	void *reg = NULL;

	if(layer > MLC_NUM_LAYERS) 
		return -EINVAL;

	if(!memregs)
		return -ENOMEM;
		
	switch(layer) {
		case 0:
		reg = memregs + MLCADDRESS0;
		break;
		case 1:
		reg = memregs + MLCADDRESS1;
		break;
		case 2:
		reg = memregs + MLCADDRESS2; /* note: weird datasheet naming MLCADDRESS3 */
		break;
	}

	iowrite32(addr, reg);
	return 0;
}

int mlc_GetHStride(u8 layer)
{
	if(layer > MLC_NUM_LAYERS || layer == MLC_VIDEO_LAYER)
		return -EINVAL;
	return ioread32(memregs+MLCHSTRIDE0+layer*0x34);
}

int mlc_SetHStride(u8 layer, u32 hstride)
{
	if(layer > MLC_NUM_LAYERS || layer == MLC_VIDEO_LAYER)
		return -EINVAL;

	//hstride &= 0x7FFFFFFF;
	iowrite32(hstride, memregs + MLCHSTRIDE0);


	return 0;
}

int mlc_SetVStride(u8 layer, u32 vstride)
{
	void *reg = NULL;

	if(layer > MLC_NUM_LAYERS)
		return -EINVAL;

	switch(layer) {
		case 0:
		reg = memregs+MLCVSTRIDE0;
		break;
		case 1:
		reg = memregs+MLCVSTRIDE1;
		break;
		case 2:
		reg = memregs+MLCVSTRIDE2; /* note: weird datasheet naming MLCVSTRIDE3*/
		break;
	}
	
	iowrite32(vstride, reg);
	
	  if (layer == MLC_VIDEO_LAYER) {
		iowrite32(vstride, memregs+MLCSTRIDECB);
		iowrite32(vstride, memregs+MLCSTRIDECR);
	}
	
	return 0;
}

int mlc_GetVStride(u8 layer)
{
	void *reg = NULL;

	if(layer > MLC_NUM_LAYERS)
		return -EINVAL;

	switch(layer) {
		case 0:
		reg = memregs+MLCVSTRIDE0;
		break;
		case 1:
		reg = memregs+MLCVSTRIDE1;
		break;
		case 2:
		reg = memregs+MLCVSTRIDE2; /* note: weird datasheet naming - they have MLCSTRIDE3*/
		break;
	}
	return ioread32(reg);
}


int mlc_SetLockSize(u8 layer, u32 locksize)
{
	u32 tmp;
	void *reg;

	/* make sure we're working with a valid lock size */
	if(!(locksize == 4 || locksize == 8 || locksize == 16))
		return -EINVAL;

	if(layer == MLC_VIDEO_LAYER)
		return -EINVAL;

	reg = SelectLayerControl(layer);
	tmp = ioread32(reg);
	tmp &= ~(3<<LOCKSIZE);
	tmp |= ((locksize/8)<<LOCKSIZE);
	iowrite32(tmp,reg);
	return 0;
}

int mlc_Set3DEnable(u8 layer, u8 en)
{
	void *reg;
	u32 tmp;

	if(layer > MLC_NUM_LAYERS || layer == MLC_VIDEO_LAYER)
		return -EINVAL;

	reg = SelectLayerControl(layer);
	tmp = ioread32(reg);

	en ? BIT_SET(tmp,GRP3DENB) : BIT_CLR(tmp,GRP3DENB);
	iowrite32(tmp, reg);
	return 0;
}

int mlc_Get3DEnable(u8 layer, int *en) /*original comment said: FIXME*/
{
	u32 tmp;
	void *reg;

	if(layer > MLC_NUM_LAYERS || layer == MLC_VIDEO_LAYER)
		return -EINVAL;

	reg = SelectLayerControl(layer);
	tmp = ioread32(reg);
	*en = IS_SET(tmp,GRP3DENB) ? 1 : 0;
	return 0;
}

u32 mlc_GetBackground(void)
{
	return ioread32(memregs+MLCBGCOLOR);
}

void mlc_SetBackground(u32 color)
{
	iowrite32((0xFFFFFF & color),memregs+MLCBGCOLOR);
}

u32 mlc_GetLayerPriority(void)
{
	u32 tmp = ioread32(memregs+MLCCONTROLT);
	return ((tmp & (0x3<<PRIORITY))>>PRIORITY);
}

int mlc_SetLayerPriority(u32 priority)
{
	u32 tmp;

	if(priority >= VID_PRIORITY_INVALID)
		return -EINVAL;

	tmp = ioread32(memregs+MLCCONTROLT);
	tmp &= ~(0x3<<PRIORITY);
	tmp |= (priority<<PRIORITY);
	iowrite32(tmp,memregs+MLCCONTROLT);
	return 0;
}


void mlc_SetTopDirtyFlag(void)
{
	u32 tmp = ioread32(memregs+MLCCONTROLT);

	BIT_SET(tmp,DITTYFLAG);
	iowrite32(tmp,memregs+MLCCONTROLT);
}

int mlc_SetScreenSize(u32 width, u32 height)
{
	if( width-1 >= 4096 || height-1 >= 4096 )
		return -EINVAL;

	iowrite32((((height-1)<<SCREENHEIGHT)|((width-1)<<SCREENWIDTH)),
				memregs+MLCSCREENSIZE);
	return 0;
}

void mlc_GetScreenSize(struct mlc_screen_size *size)
{
	u32 tmp = ioread32(memregs+MLCSCREENSIZE);

	size->width  = ((tmp & (0x7FF<<SCREENWIDTH))>>SCREENWIDTH)+1;
	size->height = ((tmp & (0x7FF<<SCREENHEIGHT))>>SCREENHEIGHT)+1;
}



int mlc_SetFormat(u8 layer, enum RGBFMT format)
{
	u32 tmp;
	void *reg;

	if(layer > MLC_NUM_LAYERS || layer == MLC_VIDEO_LAYER || 
			format > 0xFFFF)
		return -EINVAL;

	reg = SelectLayerControl(layer);
	tmp = ioread32(reg);
	tmp &= ~(0xFFFF<<FORMAT); /* clear format bits */
	tmp |= (format<<FORMAT); /* set format */
	iowrite32(tmp,reg);
	return 0;
}

int mlc_GetFormat(u8 layer, int *format)
{
	u32 tmp;
	void *reg;

	if(layer > MLC_NUM_LAYERS || layer == MLC_VIDEO_LAYER)
		return -EINVAL;

	reg = SelectLayerControl(layer);
	tmp = ioread32(reg);
	*format = ((tmp & (0xFFFF<<FORMAT))>>FORMAT);
	return 0;
}

int mlc_SetPosition(u8 layer, s32 top, s32 left, s32 right, s32 bottom)
{
	if(layer > MLC_NUM_LAYERS)
		return -EINVAL;

	right--;
	bottom--;

	top &= 0x7FF;
	left &= 0x7FF;
	right &= 0x7FF;
	bottom &= 0x7FF;

	iowrite32(((left<<LEFT)|(right<<RIGHT)),
			memregs+MLCLEFTRIGHT0+0x34*layer);
	iowrite32(((top<<TOP)|(bottom<<BOTTOM)),
			memregs+MLCTOPBOTTOM0+0x34*layer);
	return 0;
}

int mlc_GetPosition(u8 layer, struct mlc_layer_position *p)
{
	u32 tmp;

	if(layer > MLC_NUM_LAYERS)
		return -EINVAL;

	tmp = ioread32(memregs+MLCLEFTRIGHT0+0x34*layer);
	p->left = ((tmp & (0x7FF<<LEFT))>>LEFT);
	p->right  = ((tmp & (0x7FF<<RIGHT))>>RIGHT);

	tmp = ioread32(memregs+MLCTOPBOTTOM0+0x34*layer);
	p->top  = ((tmp & (0x7FF<<TOP))>>TOP);
	p->bottom = ((tmp & (0x7FF<<BOTTOM))>>BOTTOM);
	return 0;
}

int mlc_SetDirtyFlag(u8 layer)
{
	void *reg;
	u32 tmp;

	if(layer > MLC_NUM_LAYERS)
		return -EINVAL;

	reg = SelectLayerControl(layer);
	tmp = ioread32(reg);
	BIT_SET(tmp,DIRTYFLAG);

	iowrite32(tmp,reg);
	return 0;
}

int mlc_GetDirtyFlag(u8 layer)
{
	void *reg;
	u32 tmp, ret=false;

	if(layer > MLC_NUM_LAYERS)
		return -EINVAL;

	reg = SelectLayerControl(layer);
	tmp = ioread32(reg);
	ret = IS_SET(tmp,DIRTYFLAG) ? 1 : 0;

	return ret;
}


int mlc_SetTransparencyAlpha(u8 layer, u8 alpha)
{
	u32 tmp;
	void *reg;

	if(layer > MLC_NUM_LAYERS)
		return -EINVAL;

	//if (layer == MLC_VIDEO_LAYER) 
	//	reg = memregs+MLCTPCOLOR2;
	//else
	//	reg = memregs+MLCTPCOLOR0+layer*0x34;
		
	switch(layer) {
		case 0:
		reg = memregs+MLCTPCOLOR0;
		break;
		case 1:
		reg = memregs+MLCTPCOLOR1;
		break;
		case 2:
		reg = memregs+MLCTPCOLOR2; /* note: weird datasheet naming - says MLCTPCOLOR3 */
		break;
	}
	
	tmp = ioread32(reg);
	tmp &= ~(0xF<<ALPHA);
	tmp |= ((0xF & alpha)<<ALPHA);
	iowrite32(tmp,reg);
	return 0;
}


int mlc_GetTransparencyAlpha(u8 layer)
{
	u32 tmp;
	void *reg;

	if(layer > MLC_NUM_LAYERS)
		return -EINVAL;

	//if (layer == MLC_VIDEO_LAYER) 
	//	reg = mlc.mem+MLCTPCOLOR3;
	//else
	//	reg = mlc.mem+MLCTPCOLOR0+layer*0x34;
	switch(layer) {
		case 0:
		reg = memregs+MLCTPCOLOR0;
		break;
		case 1:
		reg = memregs+MLCTPCOLOR1;
		break;
		case 2:
		reg = memregs+MLCTPCOLOR2; /* note: weird datasheet naming - says MLCTPCOLOR3 */
		break;
	}
	tmp = ioread32(reg);
	return ((tmp & (0xF<<ALPHA))>>ALPHA);
}


int mlc_SetTransparencyColor(u8 layer, u32 color)
{
	u32 tmp;
	void *reg;

	if(layer > MLC_NUM_LAYERS || layer == MLC_VIDEO_LAYER)
		return -EINVAL;

	//reg = SelectLayerControl(layer);
    //reg = SelectLayerControl(layer) + MLCTPCOLOR0 - MLCCONTROL0;
	switch(layer) {
		case 0:
		reg = memregs+MLCTPCOLOR0;
		break;
		case 1:
		reg = memregs+MLCTPCOLOR1;
		break;
		case 2:
		reg = memregs+MLCTPCOLOR2; /* note: weird datasheet naming - says MLCTPCOLOR3 */
		break;
	}
	
	tmp = ioread32(reg);
	tmp &= ~(0xFFFFFF<<TPCOLOR);
	tmp |= ((0xFFFFFF & color)<<TPCOLOR);
	iowrite32(tmp,reg);
	return 0;
}

int mlc_GetTransparencyColor(u8 layer, int *color)
{
	u32 tmp;
	void *reg;

	if(layer > MLC_NUM_LAYERS || layer == MLC_VIDEO_LAYER)
		return -EINVAL;
	switch(layer) {
		case 0:
		reg = memregs+MLCTPCOLOR0;
		break;
		case 1:
		reg = memregs+MLCTPCOLOR1;
		break;
		case 2:
		reg = memregs+MLCTPCOLOR2; /* note: weird datasheet naming - says MLCTPCOLOR3 */
		break;
	}
//	reg = SelectLayerControl(layer);
//should be reg = SelectLayerControl(layer) + MLCTPCOLOR0 - MLCCONTROL0;




	tmp = ioread32(reg);
	*color = ((tmp & (0xFFFFFF<<TPCOLOR))>>TPCOLOR);
	return 0;
}


int mlc_SetBlendEnable(u8 layer, u8 en)
{
	u32 tmp;
	void *reg;

	if(layer > MLC_NUM_LAYERS)
		return -EINVAL;

	reg = SelectLayerControl(layer);
	tmp = ioread32(reg);
	en ? BIT_SET(tmp,BLENDENB) : BIT_CLR(tmp,BLENDENB);
	iowrite32(tmp,reg);
	return 0;
}

int mlc_GetBlendEnable(u8 layer, int *en) /* original comment said FIXME*/
{
	u32 tmp;
	void *reg;

	if(layer > MLC_NUM_LAYERS)
		return -EINVAL;

	reg = SelectLayerControl(layer);
	tmp = ioread32(reg);
	*en = IS_SET(tmp,BLENDENB) ? 1 : 0;
	return 0;
}


int mlc_SetTransparencyEnable(u8 layer, u8 en)
{
	u32 tmp;
	void *reg;

	if(layer > MLC_NUM_LAYERS || layer == MLC_VIDEO_LAYER)
		return -EINVAL;

	reg = SelectLayerControl(layer);
	tmp = ioread32(reg);
	en ? BIT_SET(tmp,TPENB) : BIT_CLR(tmp,TPENB);
	iowrite32(tmp,reg);
	return 0;
}

int mlc_GetTransparencyEnable(u8 layer, int *en)
{
	u32 tmp;
	void *reg;

	if(layer > MLC_NUM_LAYERS || layer == MLC_VIDEO_LAYER)
		return -EINVAL;

	reg = SelectLayerControl(layer);

	tmp = ioread32(reg);
	*en = IS_SET(tmp,TPENB) ? 1 : 0;
	return 0;
}


int mlc_SetInvertEnable(u8 layer, u8 en)
{
	u32 tmp;
	void *reg;

	if(layer > MLC_NUM_LAYERS || layer == MLC_VIDEO_LAYER) 
		return -EINVAL;

	reg = SelectLayerControl(layer);

	tmp = ioread32(reg);
	en ? BIT_SET(tmp,INVENB) : BIT_CLR(tmp,INVENB);
	iowrite32(tmp,reg);
	return 0;
}

int mlc_GetInvertEnable(u8 layer, int *en)
{
	u32 tmp;
	void *reg;

	if(layer > MLC_NUM_LAYERS || layer == MLC_VIDEO_LAYER)
		return -EINVAL;

	reg = SelectLayerControl(layer);

	tmp = ioread32(reg);
	*en = IS_SET(tmp,INVENB) ? 1 : 0;
	return 0;
}

int mlc_SetInvertColor(u8 layer, u32 color)
{
	u32 tmp;
	void *reg;

	if(layer > MLC_NUM_LAYERS || layer == MLC_VIDEO_LAYER)
		return -EINVAL;

	//reg = mlc.mem+MLCINVCOLOR0+layer*0x34;
		switch(layer) {
		case 0:
		reg = memregs+MLCINVCOLOR0;
		break;
		case 1:
		reg = memregs+MLCINVCOLOR1;
		break;
		case 2:
		reg = memregs+MLCINVCOLOR2; 
		break;
	}
	
	tmp = ioread32(reg);
	tmp &= ~(0xFFFFFF<<INVCOLOR);
	tmp |= ((0xFFFFFF & color)<<INVCOLOR);
	iowrite32(tmp,reg);
	return 0;
}


int mlc_GetInvertColor(u8 layer, int *color)
{
	u32 tmp;
	void *reg;

	if(layer > MLC_NUM_LAYERS || layer == MLC_VIDEO_LAYER)
		return -EINVAL;

	//reg = mlc.mem+MLCINVCOLOR0+layer*0x34;
	switch(layer) {
		case 0:
		reg = memregs+MLCINVCOLOR0;
		break;
		case 1:
		reg = memregs+MLCINVCOLOR1;
		break;
		case 2:
		reg = memregs+MLCINVCOLOR2; 
		break;
	}

	tmp = ioread32(reg);
	*color = ((tmp & (0xFFFFFF<<INVCOLOR))>>INVCOLOR);
	return 0;
}


int mlc_SetOverlaySize(u8 layer, u32 srcwidth, u32 srcheight, u32 dstwidth, 
		u32 dstheight)
{
	/* Enable adjusted ratio with bilinear filter for upscaling */
	if (srcwidth < dstwidth)
		iowrite32((1<<28) | (((srcwidth-1)<<11)/(dstwidth-1)), memregs+MLCHSCALE);
	else
		iowrite32((srcwidth<<11)/(dstwidth), memregs+MLCHSCALE);
	/* Ditto for height which scales independently of width */
	if (srcheight < dstheight)	
		iowrite32((1<<28) | (((srcheight-1)<<11)/(dstheight-1)), memregs+MLCVSCALE);
	else
		iowrite32((srcheight<<11)/(dstheight), memregs+MLCVSCALE);
	return 0;
}

int mlc_GetOverlaySize(u8 layer, struct mlc_overlay_size *psize)
{
	u32 hscale = ioread32(memregs+MLCHSCALE);
	u32 vscale = ioread32(memregs+MLCVSCALE);

	psize->srcwidth = (hscale>>11) & 0x7FF;
	psize->srcheight = hscale & 0x7FF;
	psize->dstwidth = (vscale>>11) & 0x7FF;
	psize->dstheight = vscale & 0x7FF;

	return 0;
}

int mlc_SetLayerInvisibleAreaEnable(u8 layer, u8 en) /***********************VALIDATE?*/
{
	u32 tmp;
	void *reg;

	if(layer > MLC_NUM_LAYERS || layer == MLC_VIDEO_LAYER)
		return -EINVAL;
		
	switch(layer) {
		case 0:
		reg = memregs+MLCLEFTRIGHT0_0;
		break;
		case 1:
		reg = memregs+MLCLEFTRIGHT1_0;
		break;
	}

	//reg = memregs+MLCLEFTRIGHT0_0+layer*0x34;
	tmp = ioread32(reg);
	en ? BIT_SET(tmp, INVALIDENB) : BIT_CLR(tmp, INVALIDENB);
	iowrite32(tmp, reg);

	return 0;
}

int mlc_GetLayerInvisibleAreaEnable(u8 layer) /***********************VALIDATE?*/
{
	u32 tmp;
	void *reg;

	if(layer > MLC_NUM_LAYERS || layer == MLC_VIDEO_LAYER)
		return -EINVAL;

	//reg = memregs+MLCLEFTRIGHT0_0+layer*0x34;
	
	switch(layer) {
		case 0:
		reg = memregs+MLCLEFTRIGHT0_0;
		break;
		case 1:
		reg = memregs+MLCLEFTRIGHT1_0;
		break;
	}
	tmp = ioread32(reg);

	return IS_SET(tmp, INVALIDENB) ? 1 : 0;
}

int mlc_SetLayerInvisibleArea(u8 layer, s32 top, s32 left, s32 right, s32 bottom)
{
	u32 tmp;
	void *reg;

	if(layer > MLC_NUM_LAYERS || layer == MLC_VIDEO_LAYER)
		return -EINVAL;

	top &= 0x7FF;
	left &= 0x7FF;
	right &= 0x7FF;
	bottom &= 0x7FF;

	//reg = memregs+MLCLEFTRIGHT0_0+layer*0x34;
	
	switch(layer) {
		case 0:
		reg = memregs+MLCLEFTRIGHT0_0;
		break;
		case 1:
		reg = memregs+MLCLEFTRIGHT1_0;
		break;
	}
	tmp = ioread32(reg);
	tmp &= ~((0x7FF<<INVALIDLEFT)|(0x7FF<<INVALIDRIGHT));
	tmp |= (left<<INVALIDLEFT)|(right<<INVALIDRIGHT);
	iowrite32(tmp, reg);

	//reg = memregs+MLCTOPBOTTOM0_0+layer*0x34;
	switch(layer) {
		case 0:
		reg = memregs+MLCTOPBOTTOM0_0;
		break;
		case 1:
		reg = memregs+MLCTOPBOTTOM1_0;
		break;
	}
	
	tmp = ioread32(reg);
	tmp &= ~((0x7FF<<INVALIDTOP)|(0x7FF<<INVALIDBOTTOM));
	tmp |= (left<<INVALIDTOP)|(right<<INVALIDBOTTOM);
	iowrite32(tmp, reg);

	return 0;
}


int mlc_GetLayerInvisibleArea(u8 layer, struct mlc_layer_position *p)
{
	u32 tmp;

	if(layer > MLC_NUM_LAYERS || layer == MLC_VIDEO_LAYER)
		return -EINVAL;

	//tmp = ioread32(mlc.mem+MLCLEFTRIGHT0_0+0x34*layer);
	
	switch(layer) {
		case 0:
		tmp = memregs+MLCLEFTRIGHT0_0;
		break;
		case 1:
		tmp = memregs+MLCLEFTRIGHT1_0;
		break;
	}
	p->left = ((tmp & (0x7FF<<LEFT))>>LEFT);
	p->right  = ((tmp & (0x7FF<<RIGHT))>>RIGHT);

	//tmp = ioread32(mlc.mem+MLCTOPBOTTOM0_0+0x34*layer);
	switch(layer) {
		case 0:
		tmp = memregs+MLCTOPBOTTOM0_0;
		break;
		case 1:
		tmp = memregs+MLCTOPBOTTOM1_0;
		break;
	}
	p->top  = ((tmp & (0x7FF<<TOP))>>TOP);
	p->bottom = ((tmp & (0x7FF<<BOTTOM))>>BOTTOM);

	return 0;
}


int mlc_SetAddressCb(u8 layer, u32 addr)
{
	if (layer != MLC_VIDEO_LAYER) 
		return -EINVAL;
	iowrite32(addr, memregs+MLCADDRESSCB);
	return 0;
}




int mlc_SetAddressCr(u8 layer, u32 addr)
{
	if (layer != MLC_VIDEO_LAYER) 
		return -EINVAL;
	iowrite32(addr, memregs+MLCADDRESSCR);
	return 0;
}






/*
 * platform device
 */

struct fb_ops lf1000fb_ops = {
	.owner		= THIS_MODULE,
	.fb_setcolreg	= lf1000fb_setcolreg,
	.fb_fillrect	= cfb_fillrect,
	.fb_copyarea	= cfb_copyarea,
	.fb_imageblit	= cfb_imageblit,
	.fb_ioctl	= lf1000fb_ioctl,
};




static int __init lf1000fb_probe(struct platform_device *pdev)
{
	struct lf1000fb_info *fbi;
	int ret = 0;
	//printk(KERN_INFO "%u\n", (unsigned int)&pdev->dev);
	printk(KERN_INFO "lf1000fb: loading\n");

	fbi = framebuffer_alloc(sizeof(struct lf1000fb_info), &pdev->dev);
	printk(KERN_INFO "lf1000fb: alloc done\n");
	if(fbi == NULL) {
		printk(KERN_ERR "lf1000fb: out of memory\n");
		return -ENOMEM;
	}
	platform_set_drvdata(pdev, fbi);


	printk(KERN_INFO "lf1000fb: ioremap_nocache\n");
	fbi->fbmem = ioremap_nocache(mlc_fb_addr, mlc_fb_size);
	printk(KERN_INFO "lf1000fb: ioremap_nocache done\n");
	if(fbi->fbmem == NULL) {
		ret = -ENOMEM;
		goto fail_fbmem;
	}

	/* configure framebuffer */
	printk(KERN_INFO "Configure Framebuffer\n");
	fbi->fb.fix = lf1000fb_fix;

	/* we have a static mode: 320x240, 32-bit */

	fbi->fb.var.xres		= X_RESOLUTION;
	fbi->fb.var.yres		= Y_RESOLUTION;
	fbi->fb.var.xres_virtual	= fbi->fb.var.xres;
	fbi->fb.var.yres_virtual	= fbi->fb.var.yres;
	fbi->fb.var.xoffset		= 0;
	fbi->fb.var.yoffset		= 0;

	switch(BYTESPP) {
		case 1:
		/*565 - 8 bits*/
			fbi->fb.var.bits_per_pixel	= BITSPP;
			fbi->fb.var.red.offset		= 0;
			fbi->fb.var.red.length		= 8;
			fbi->fb.var.green.offset	= 0;
			fbi->fb.var.green.length	= 8;
			fbi->fb.var.blue.offset		= 0;
			fbi->fb.var.blue.length		= 8;
			fbi->fb.var.transp.offset	= 0;
			fbi->fb.var.transp.length	= 0;
			break;
		case 2:
		/*565 - 16 bits*/
			fbi->fb.var.bits_per_pixel	= BITSPP;
			fbi->fb.var.red.offset		= 5;//was 11
			fbi->fb.var.red.length		= 5;
			fbi->fb.var.green.offset	= 5;
			fbi->fb.var.green.length	= 6;
			fbi->fb.var.blue.offset		= 0;
			fbi->fb.var.blue.length		= 5;
			fbi->fb.var.transp.offset	= 0;
			fbi->fb.var.transp.length	= 0;
			break;
		case 3:
		/*888 24bits*/
			fbi->fb.var.bits_per_pixel	= BITSPP;
			fbi->fb.var.red.offset		= 0;//0 for bgr. 16 for rgb
			fbi->fb.var.red.length		= 8;
			fbi->fb.var.green.offset	= 8;
			fbi->fb.var.green.length	= 8;
			fbi->fb.var.blue.offset		= 16;//16 for bgr. 0 for rgb
			fbi->fb.var.blue.length		= 8;
			fbi->fb.var.transp.offset	= 0;
			fbi->fb.var.transp.length	= 0;
			break;
		case 4:
		/* 8888 32bits*/
			fbi->fb.var.bits_per_pixel	= BITSPP;
			fbi->fb.var.red.offset		= 16;
			fbi->fb.var.red.length		= 8;
			fbi->fb.var.green.offset	= 8;
			fbi->fb.var.green.length	= 8;
			fbi->fb.var.blue.offset		= 0;
			fbi->fb.var.blue.length		= 8;
			fbi->fb.var.transp.offset	= 0;
			fbi->fb.var.transp.length	= 0;
			break;
	}
	set_mlc(pdev);

	fbi->fb.var.nonstd		= 0;
	fbi->fb.var.activate		= FB_ACTIVATE_NOW;
	fbi->fb.var.height		= -1;
	fbi->fb.var.width		= -1;
	fbi->fb.var.vmode		= FB_VMODE_NONINTERLACED;

	fbi->fb.fbops			= &lf1000fb_ops;
	fbi->fb.flags			= FBINFO_DEFAULT;
	fbi->fb.node			= -1;

	printk(KERN_INFO "lf1000fb: setting pseudopallette\n");
	fbi->fb.pseudo_palette		= fbi->pseudo_pal;

	printk(KERN_INFO "lf1000fb: fbi->pseudopallete done\n");
	fbi->fb.screen_size		= mlc_fb_size;
	fbi->fb.screen_base		= fbi->fbmem;

	memset(fbi->pseudo_pal, PALETTE_CLEAR, ARRAY_SIZE(fbi->pseudo_pal));

	printk(KERN_INFO "lf1000fb: fb_alloc_cmap\n");

	fb_alloc_cmap(&fbi->fb.cmap, 1<<fbi->fb.var.bits_per_pixel, 0);

	printk(KERN_INFO "lf1000fb: fb_alloc_cmap done\n");
	printk(KERN_INFO "lf1000fb: register FB\n");
	ret = register_framebuffer(&fbi->fb);
	printk(KERN_INFO "lf1000fb: register FB done\n");

	if(ret < 0) {
		dev_err(&pdev->dev, "Failed to register FB device: %d\n", ret);
		goto fail_register;
	}

	return 0;

fail_register:
	iounmap(fbi->fbmem);
	fb_dealloc_cmap(&fbi->fb.cmap);
	framebuffer_release(&fbi->fb);
	platform_set_drvdata(pdev, NULL);
fail_fbmem:
	kfree(fbi);
	return ret;
}

static int lf1000fb_remove(struct platform_device *pdev)
{
	struct lf1000fb_info *fbi = platform_get_drvdata(pdev);
	
	printk(KERN_INFO "lf1000fb: unloading\n");

	iounmap(fbi->fbmem);

	unregister_framebuffer(&fbi->fb);
	fb_dealloc_cmap(&fbi->fb.cmap);
	framebuffer_release(&fbi->fb);

	kfree(fbi);
	return 0;
}

static struct platform_driver lf1000fb_driver = {
	.probe		= lf1000fb_probe,
	.remove		= lf1000fb_remove,
	.driver		= {
		.name	= "lf1000-fb",
		.owner	= THIS_MODULE,
	},
};

static int lf1000fb_init(void)
{
	return platform_driver_register(&lf1000fb_driver);
}

static void __exit lf1000fb_exit(void)
{
	platform_driver_unregister(&lf1000fb_driver);
}



module_init(lf1000fb_init);
module_exit(lf1000fb_exit);

MODULE_AUTHOR("Andrey Yurovsky");
MODULE_AUTHOR("Michael Gold");
MODULE_LICENSE("GPL");
