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



#define MLCCONTROLT             0x00
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


/* MLC RGB Layer n Control Register (MLCCONTROLn) */
#define GRP3DENB                8       /* set layer as output of 3D core */
#define LAYERENB                5       /* enable the layer */
#define PALETTEPWD              15      /* layer n palette table on/off */
#define PALETTESLD              14      /* layer n palette table sleep mode */
#define MLC_NUM_LAYERS			3
#define MLC_VIDEO_LAYER		(MLC_NUM_LAYERS-1)

/* MLC TOP CONTROL REGISTER (MLCCONTROLT) */
#define DITTYFLAG               3
#define PIXELBUFFER_SLD         10      /* pixel buffer sleep mode */
#define PIXELBUFFER_PWD         11      /* pixel buffer power on/off */
#define MLCENB                  1

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

#define MLC_IOC_MAGIC   'm'
#define MLC_IOCTBACKGND		_IO(MLC_IOC_MAGIC,  1)
#define MLC_IOCTLAYEREN		_IO(MLC_IOC_MAGIC,  8)
#define MLC_IOCTADDRESS		_IO(MLC_IOC_MAGIC,  9)
#define MLC_IOCTHSTRIDE		_IO(MLC_IOC_MAGIC,  10)
#define MLC_IOCTVSTRIDE		_IO(MLC_IOC_MAGIC,  11)
#define MLC_IOCT3DENB		_IO(MLC_IOC_MAGIC,  18)
#define MLC_IOCTDIRTY		_IO(MLC_IOC_MAGIC,  17)
#define MLC_IOCQDIRTY		_IO(MLC_IOC_MAGIC,  32)
#define MLC_IOCQHSTRIDE		_IO(MLC_IOC_MAGIC,  26)
#define MLC_IOCQADDRESS		_IO(MLC_IOC_MAGIC,  25)

//int mlc_layer_ioctl(struct inode *inode, struct file *filp, unsigned int cmd,unsigned long arg)
//static int pollux_ioctl(struct fb_info *info, unsigned int cmd, unsigned long arg)

static int lf1000fb_ioctl(struct fb_info *info, unsigned int cmd, unsigned long arg)
{

	int result = 0;
	void __user *argp = (void __user *)arg;
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
		case MLC_IOCTBACKGND:
		mlc_SetBackground(arg);
		break;

		case MLC_IOCTLAYEREN:
		result = mlc_SetLayerEnable(layerID, arg);
		break;

		case MLC_IOCTADDRESS:
		result = mlc_SetAddress(layerID, arg);
		break;

		case MLC_IOCTHSTRIDE:
		result = mlc_SetHStride(layerID, arg);
		break;

		case MLC_IOCTVSTRIDE:
		result = mlc_SetVStride(layerID, arg);
		break;

		case MLC_IOCT3DENB:
		result = mlc_Set3DEnable(layerID, arg);
		break;

		case MLC_IOCTDIRTY:
		result = mlc_SetDirtyFlag(layerID);
		break;

		case MLC_IOCQDIRTY:
		result = mlc_GetDirtyFlag(layerID);
		break;
		
		case MLC_IOCQHSTRIDE:
		result = mlc_GetHStride(layerID);
		break;
		
		case MLC_IOCQADDRESS:
		if(mlc_GetAddress(layerID, &result) < 0)
		return -EFAULT;
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

void mlc_SetBackground(u32 color)
{
	iowrite32((0xFFFFFF & color),memregs+MLCBGCOLOR);
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



struct fb_ops lf1000fb_ops = {
	.owner		= THIS_MODULE,
	.fb_setcolreg	= lf1000fb_setcolreg,
	.fb_fillrect	= cfb_fillrect,
	.fb_copyarea	= cfb_copyarea,
	.fb_imageblit	= cfb_imageblit,
	.fb_ioctl	= lf1000fb_ioctl,
};



/*
 * platform device
 */

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
