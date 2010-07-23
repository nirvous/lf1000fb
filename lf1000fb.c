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
#define MLCBGCOLOR			0x8 //0x4008
#define DIRTYFLAG			4
#define LAYERENB			5				
#define FORMAT				16	/* see table 20-5 */


//#define BGCOLOR				0xFFFFFF  //RGBCODE
#define BYTESPP				4
#define BITSPP				BYTESPP*8
#define VISUALTYPE			FB_VISUAL_TRUECOLOR //FB_VISUAL_PSEUDOCOLOR, FB_VISUAL_TRUECOLOR
#define FORMATCODE			0x0653

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
static void set_format(struct platform_device *pdev)
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
	
	printk(KERN_INFO "lf1000fb: Current MLC Mode: 0x%X\n", (ioread32(memregs+MLCCONTROL0)>>FORMAT) & 0xFFFF);
	
	reg=ioread32(memregs+MLCCONTROL0) & ~(0xFFFF<<FORMAT);
	iowrite32(reg | (FORMATCODE<<FORMAT), memregs+MLCCONTROL0); /*Write the format */
	printk(KERN_INFO "lf1000fb: New MLC Mode: 0x%X\n", (ioread32(memregs+MLCCONTROL0)>>FORMAT) & 0xFFFF);

		
	hstride = BYTESPP;							
	iowrite32(hstride, memregs + MLCHSTRIDE0);
	printk(KERN_INFO "lf1000fb: New HStride: %d\n", (ioread32(memregs + MLCHSTRIDE0)));
	
	
	vstride = hstride*X_RESOLUTION;
	iowrite32(vstride, memregs + MLCVSTRIDE0);
	printk(KERN_INFO "lf1000fb: New VStride: %d\n", (ioread32(memregs + MLCVSTRIDE0)));

	dirtyflag = ioread32(memregs+MLCCONTROL0);						/*Read the register*/
	iowrite32(dirtyflag | (1<<DIRTYFLAG), memregs+MLCCONTROL0);		/*Set dirty*/

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

struct fb_ops lf1000fb_ops = {
	.owner		= THIS_MODULE,
	.fb_setcolreg	= lf1000fb_setcolreg,
	.fb_fillrect	= cfb_fillrect,
	.fb_copyarea	= cfb_copyarea,
	.fb_imageblit	= cfb_imageblit,
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
	set_format(pdev);

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
