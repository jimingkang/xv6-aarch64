/* S5PV210 FIMD LCD (lcd.c). The frame buffer lives at DRAM_BASE +
   LCD_FB_OFFSET, just above the memory given to the page allocator. */
#define LCD_FB_OFFSET (128U<<20)
#define LCD_FB_SIZE (2U<<20)
void lcd_init(void);
void lcd_putc(int c);
unsigned int lcd_fb_phys(void);
