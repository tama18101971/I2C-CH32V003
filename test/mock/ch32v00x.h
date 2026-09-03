/*
 * test/mock/ch32v00x.h — замена SDK-заголовка для хост-тестов драйвера.
 *
 * Подставляется вместо настоящего ch32v00x.h через -Itest/mock, поэтому
 * src/i2c.c компилируется на хосте БЕЗ единой правки (как C++, чтобы работали
 * перехватчики регистров).
 *
 * Ключевой приём: поля регистров — не переменные, а объекты с перегруженными
 * operator T() / operator= / operator|= / operator&=. Поэтому модель видит
 * КАЖДОЕ обращение драйвера с точностью до регистра и направления (чтение или
 * запись). Это позволяет смоделировать реальную семантику периферии: сброс ADDR
 * чтением STAR1→STAR2, очистку RXNE/BTF чтением DATAR, очистку AF записью
 * инвертированной маски в STAR1 и т. д.
 *
 * Раскладка полей и значения битов совпадают с ch32v00x.h из WCH NoneOS SDK.
 */

#ifndef MOCK_CH32V00X_H
#define MOCK_CH32V00X_H

#include <stdint.h>

#ifndef __cplusplus
#error "test/mock/ch32v00x.h requires a C++ compiler (register access is intercepted via operator overloading)"
#endif

/* Идентификаторы перехватываемых регистров */
enum MockRegId {
    MOCK_I2C_CTLR1 = 0,
    MOCK_I2C_CTLR2,
    MOCK_I2C_OADDR1,
    MOCK_I2C_OADDR2,
    MOCK_I2C_DATAR,
    MOCK_I2C_STAR1,
    MOCK_I2C_STAR2,
    MOCK_I2C_CKCFGR,
    MOCK_GPIOC_CFGLR,
    MOCK_GPIOC_CFGHR,
    MOCK_GPIOC_INDR,
    MOCK_GPIOC_OUTDR,
    MOCK_GPIOC_BSHR,
    MOCK_GPIOC_BCR,
    MOCK_RCC_APB1PCENR,
    MOCK_RCC_APB2PCENR,
    MOCK_REG_COUNT
};

extern "C" {
uint32_t mock_reg_read(int id);
void mock_reg_write(int id, uint32_t value);
void mock_disable_irq(void);
void mock_enable_irq(void);
extern uint32_t SystemCoreClock;
}

/*
 * Перехватчик регистра. Операторы |= и &= шаблонные по типу аргумента: драйвер
 * пишет `reg &= ~BIT`, где ~BIT после целочисленного продвижения имеет тип int,
 * и приём такого аргумента без сужения избавляет от ложных -Woverflow
 * (на целевой платформе усечение до uint16_t выполняет само присваивание
 * volatile-полю).
 */
template <int ID, typename T>
struct MockRegister {
    operator T() const { return (T)mock_reg_read(ID); }

    MockRegister &operator=(T v) {
        mock_reg_write(ID, (uint32_t)v);
        return *this;
    }

    template <typename U>
    MockRegister &operator|=(U v) {
        T next = (T)(((T)mock_reg_read(ID)) | (T)v);
        mock_reg_write(ID, (uint32_t)next);
        return *this;
    }

    template <typename U>
    MockRegister &operator&=(U v) {
        T next = (T)(((T)mock_reg_read(ID)) & (T)v);
        mock_reg_write(ID, (uint32_t)next);
        return *this;
    }
};

struct I2C_TypeDef {
    MockRegister<MOCK_I2C_CTLR1,  uint16_t> CTLR1;
    MockRegister<MOCK_I2C_CTLR2,  uint16_t> CTLR2;
    MockRegister<MOCK_I2C_OADDR1, uint16_t> OADDR1;
    MockRegister<MOCK_I2C_OADDR2, uint16_t> OADDR2;
    MockRegister<MOCK_I2C_DATAR,  uint16_t> DATAR;
    MockRegister<MOCK_I2C_STAR1,  uint16_t> STAR1;
    MockRegister<MOCK_I2C_STAR2,  uint16_t> STAR2;
    MockRegister<MOCK_I2C_CKCFGR, uint16_t> CKCFGR;
};

struct GPIO_TypeDef {
    MockRegister<MOCK_GPIOC_CFGLR, uint32_t> CFGLR;
    MockRegister<MOCK_GPIOC_CFGHR, uint32_t> CFGHR;
    MockRegister<MOCK_GPIOC_INDR,  uint32_t> INDR;
    MockRegister<MOCK_GPIOC_OUTDR, uint32_t> OUTDR;
    MockRegister<MOCK_GPIOC_BSHR,  uint32_t> BSHR;
    MockRegister<MOCK_GPIOC_BCR,   uint32_t> BCR;
};

struct RCC_TypeDef {
    MockRegister<MOCK_RCC_APB1PCENR, uint32_t> APB1PCENR;
    MockRegister<MOCK_RCC_APB2PCENR, uint32_t> APB2PCENR;
};

extern I2C_TypeDef  mock_i2c1;
extern GPIO_TypeDef mock_gpioc;
extern RCC_TypeDef  mock_rcc;

#define I2C1        (&mock_i2c1)
#define GPIOC       (&mock_gpioc)
#define RCC         (&mock_rcc)

/* === Биты регистров (значения из ch32v00x.h WCH NoneOS SDK) === */

#define I2C_CTLR1_PE        ((uint16_t)0x0001)
#define I2C_CTLR1_START     ((uint16_t)0x0100)
#define I2C_CTLR1_STOP      ((uint16_t)0x0200)
#define I2C_CTLR1_ACK       ((uint16_t)0x0400)
#define I2C_CTLR1_POS       ((uint16_t)0x0800)
#define I2C_CTLR1_SWRST     ((uint16_t)0x8000)

#define I2C_STAR1_SB        ((uint16_t)0x0001)
#define I2C_STAR1_ADDR      ((uint16_t)0x0002)
#define I2C_STAR1_BTF       ((uint16_t)0x0004)
#define I2C_STAR1_RXNE      ((uint16_t)0x0040)
#define I2C_STAR1_TXE       ((uint16_t)0x0080)
#define I2C_STAR1_BERR      ((uint16_t)0x0100)
#define I2C_STAR1_ARLO      ((uint16_t)0x0200)
#define I2C_STAR1_AF        ((uint16_t)0x0400)
#define I2C_STAR1_OVR       ((uint16_t)0x0800)

#define I2C_STAR2_MSL       ((uint16_t)0x0001)
#define I2C_STAR2_BUSY      ((uint16_t)0x0002)
#define I2C_STAR2_TRA       ((uint16_t)0x0004)

#define I2C_CKCFGR_CCR      ((uint16_t)0x0FFF)
#define I2C_CKCFGR_DUTY     ((uint16_t)0x4000)
#define I2C_CKCFGR_FS       ((uint16_t)0x8000)

#define RCC_IOPCEN          ((uint32_t)0x00000010)
#define RCC_I2C1EN          ((uint32_t)0x00200000)

/* === Критические секции: модель считает вложенность === */

#define __disable_irq()     mock_disable_irq()
#define __enable_irq()      mock_enable_irq()

#endif /* MOCK_CH32V00X_H */
