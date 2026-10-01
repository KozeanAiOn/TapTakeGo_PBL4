#include <stdint.h>

// ==========================================
// ĐỊNH NGHĨA ĐỊA CHỈ THANH GHI THỦ CÔNG (BARE-METAL)
// ==========================================
#define RCC_APB2ENR  (*((volatile uint32_t *)0x40021018))
#define GPIOA_CRL    (*((volatile uint32_t *)0x40010800))
#define GPIOA_IDR    (*((volatile uint32_t *)0x40010808))
#define GPIOA_ODR    (*((volatile uint32_t *)0x4001080C))
#define GPIOA_BSRR   (*((volatile uint32_t *)0x40010810))
#define GPIOA_BRR    (*((volatile uint32_t *)0x40010814))

#define GPIOB_CRL    (*((volatile uint32_t *)0x40010C00))
#define GPIOB_ODR    (*((volatile uint32_t *)0x40010C0C))
#define GPIOB_BSRR   (*((volatile uint32_t *)0x40010C10))
#define GPIOB_BRR    (*((volatile uint32_t *)0x40010C14))

// ==========================================
// BIẾN TOÀN CỤC & HIỆU CHUẨN
// ==========================================
int32_t HX711_Offset = 0;
float HX711_Scale = 378.57f; // Hệ số Scale chuẩn 84g

// Biến cho thuật toán lọc nhiễu thích nghi (Adaptive EMA)
float filtered_weight = 0.0f;
float alpha = 0.15f;

// Biến cho tính năng đếm
uint8_t current_mode = 0; // 0: Cân khối lượng, 1: Đếm số lượng
float unit_weight = 1.0f; // Khối lượng gốc của 1 vật mẫu

// Biến Debug
volatile int32_t raw_value_no_scale = 0;
volatile float final_weight_gram = 0;
volatile int32_t item_count = 0;

// ==========================================
// HÀM DELAY CƠ BẢN (CLOCK 8MHz)
// ==========================================
void delay_us(uint32_t us) {
    uint32_t delay = us * 2;
    while(delay--) __asm("nop");
}
void delay_ms(uint32_t ms) {
    while(ms--) delay_us(1000);
}

// ==========================================
// THƯ VIỆN LCD 16x2 BARE-METAL (PORT B)
// ==========================================
#define LCD_RS_HIGH()  (GPIOB_BSRR = (1 << 0))
#define LCD_RS_LOW()   (GPIOB_BRR  = (1 << 0))
#define LCD_EN_HIGH()  (GPIOB_BSRR = (1 << 1))
#define LCD_EN_LOW()   (GPIOB_BRR  = (1 << 1))

void lcd_send_nibble(uint8_t data) {
    GPIOB_BRR = (0xF << 2);
    GPIOB_BSRR = ((data & 0x0F) << 2);
    LCD_EN_HIGH(); delay_us(50);
    LCD_EN_LOW(); delay_us(50);
}

void lcd_send_cmd(uint8_t cmd) {
    LCD_RS_LOW();
    lcd_send_nibble(cmd >> 4);
    lcd_send_nibble(cmd);
    delay_ms(2);
}

void lcd_send_data(uint8_t data) {
    LCD_RS_HIGH();
    lcd_send_nibble(data >> 4);
    lcd_send_nibble(data);
    delay_us(50);
}

void lcd_init(void) {
    RCC_APB2ENR |= (1 << 3); // Bật clock PORTB
    GPIOB_CRL &= 0xFF000000;
    GPIOB_CRL |= 0x00333333; // PB0-PB5 là Output Push-Pull

    delay_ms(50);
    LCD_RS_LOW(); LCD_EN_LOW();

    lcd_send_nibble(0x03); delay_ms(5);
    lcd_send_nibble(0x03); delay_ms(1);
    lcd_send_nibble(0x03); delay_ms(1);
    lcd_send_nibble(0x02); delay_ms(1);

    lcd_send_cmd(0x28);
    lcd_send_cmd(0x0C);
    lcd_send_cmd(0x06);
    lcd_send_cmd(0x01);
    delay_ms(5);
}

void lcd_set_cursor(uint8_t col, uint8_t row) {
    uint8_t address = (row == 0) ? 0x80 : 0xC0;
    lcd_send_cmd(address | col);
}

void lcd_print(const char* str) {
    while(*str) {
        lcd_send_data((uint8_t)(*str));
        str++;
    }
}

void lcd_print_float(float val) {
    if (val < 0) {
        lcd_send_data('-');
        val = -val;
    }
    int32_t int_part = (int32_t)val;
    int32_t frac_part = (int32_t)((val - int_part) * 10);

    char buffer[10];
    int i = 0;
    if (int_part == 0) buffer[i++] = '0';
    while (int_part > 0) {
        buffer[i++] = (int_part % 10) + '0';
        int_part /= 10;
    }
    while (i > 0) lcd_send_data(buffer[--i]);

    lcd_send_data('.');
    lcd_send_data(frac_part + '0');
}

void lcd_print_int(int32_t val) {
    char buffer[10];
    int i = 0;
    if (val == 0) buffer[i++] = '0';
    while (val > 0) {
        buffer[i++] = (val % 10) + '0';
        val /= 10;
    }
    while (i > 0) lcd_send_data(buffer[--i]);
}

// ==========================================
// CÁC HÀM XỬ LÝ HX711 & NGOẠI VI
// ==========================================
void System_Init(void) {
    RCC_APB2ENR |= (1 << 2); // Bật clock PORTA

    // PA0 (DT) - Input Pull-up
    GPIOA_CRL &= ~((uint32_t)0xF << 0);
    GPIOA_CRL |= ((uint32_t)0x8 << 0);
    GPIOA_ODR |= (1 << 0);

    // PA1 (SCK) - Output Push-Pull
    GPIOA_CRL &= ~((uint32_t)0xF << 4);
    GPIOA_CRL |= ((uint32_t)0x3 << 4);
    GPIOA_BRR = (1 << 1);

    // PA2 (Nút nhấn đếm số lượng) - Input Pull-up
    GPIOA_CRL &= ~((uint32_t)0xF << 8);
    GPIOA_CRL |= ((uint32_t)0x8 << 8);
    GPIOA_ODR |= (1 << 2);
}

int32_t HX711_Read(void) {
    uint32_t count = 0;
    while ((GPIOA_IDR & (1 << 0)) != 0);

    for (int i = 0; i < 24; i++) {
        GPIOA_BSRR = (1 << 1); delay_us(1);
        count = count << 1;
        GPIOA_BRR = (1 << 1); delay_us(1);
        if (GPIOA_IDR & (1 << 0)) count++;
    }

    GPIOA_BSRR = (1 << 1); delay_us(1);
    GPIOA_BRR = (1 << 1); delay_us(1);

    if (count & 0x800000) count |= 0xFF000000;
    return (int32_t)count;
}

int32_t HX711_Average(uint8_t times) {
    int64_t sum = 0;
    for (uint8_t i = 0; i < times; i++) sum += HX711_Read();
    return (int32_t)(sum / times);
}

void HX711_Tare(uint8_t times) {
    HX711_Offset = HX711_Average(times);
}

float HX711_GetWeight(uint8_t times) {
    int32_t current_val = HX711_Average(times);
    return (float)(current_val - HX711_Offset) / HX711_Scale;
}

// ==========================================
// CHƯƠNG TRÌNH CHÍNH
// ==========================================
int main(void) {
    System_Init();
    lcd_init();

    // Màn hình khởi động
    lcd_set_cursor(0, 0); lcd_print(" KHOI DONG CAN  ");
    lcd_set_cursor(0, 1); lcd_print(" Vui long cho...");

    delay_ms(1000);
    HX711_Tare(15); // Lấy mẫu 15 lần để bù trừ mốc 0 chính xác

    lcd_send_cmd(0x01); // Xóa màn hình

    uint8_t last_btn_state = 1;

    while (1) {
        // 1. Chỉ lấy 1 mẫu để đáp ứng tức thời
        float current_weight = HX711_GetWeight(1);

        // Cập nhật giá trị thô để Debug
        raw_value_no_scale = HX711_Average(1) - HX711_Offset;

        // 2. LỌC THÍCH NGHI (Adaptive Filter)
        float diff = current_weight - filtered_weight;
        if (diff < 0) diff = -diff; // Lấy trị tuyệt đối

        if (diff > 10.0f) {
            alpha = 1.0f;  // Biến động lớn -> Phản hồi ngay 100%
        } else {
            alpha = 0.15f; // Biến động nhỏ -> Lọc mượt để khử nhiễu
        }

        filtered_weight = (alpha * current_weight) + ((1.0f - alpha) * filtered_weight);

        // 3. KHỬ VÙNG KHÔNG (Auto-Zero Tracking)
        final_weight_gram = filtered_weight;
        if (final_weight_gram >= -5.0f && final_weight_gram <= 5.0f) {
            final_weight_gram = 0.0f;
        }

        // 4. XỬ LÝ NÚT NHẤN PA2
        uint8_t btn_state = (GPIOA_IDR & (1 << 2)) ? 1 : 0;

        if (btn_state == 0 && last_btn_state == 1) {
            lcd_send_cmd(0x01);

            if (current_mode == 0) {
                // Đang cân -> Chuyển sang đếm (lưu khối lượng vật mẫu)
                if (final_weight_gram > 5.0f) {
                    unit_weight = final_weight_gram;
                    current_mode = 1;
                }
            } else {
                // Đang đếm -> Chuyển về cân
                current_mode = 0;
                item_count = 0;
            }
        }
        last_btn_state = btn_state;

        // 5. HIỂN THỊ LCD
        if (current_mode == 0) {
            lcd_set_cursor(0, 0); lcd_print("  CAN DIEN TU   ");
            lcd_set_cursor(13, 1); lcd_print("Gam");

            lcd_set_cursor(3, 1);
            lcd_print("        ");
            lcd_set_cursor(3, 1);
            lcd_print_float(final_weight_gram);

        } else if (current_mode == 1) {
            item_count = (int32_t)((final_weight_gram / unit_weight) + 0.5f);
            if (item_count < 0) item_count = 0;

            lcd_set_cursor(0, 0); lcd_print(" SO LUONG VAT:  ");
            lcd_set_cursor(13, 1); lcd_print("Pcs");

            lcd_set_cursor(5, 1);
            lcd_print("      ");
            lcd_set_cursor(5, 1);
            lcd_print_int(item_count);
        }

        // Tốc độ vòng lặp cực nhanh để đáp ứng phím và cập nhật khối lượng
        delay_ms(10);
    }
}
// last update: 00:10 26/09/2026
