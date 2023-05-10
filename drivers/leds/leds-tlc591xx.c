// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2014 Belkin Inc.
 * Copyright 2015 Andrew Lunn <andrew@lunn.ch>
 */

#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>

#define TLC591XX_MAX_LEDS	16
#define TLC591XX_MAX_BRIGHTNESS	256

#define TLC591XX_REG_MODE1	0x00
#define MODE1_RESPON_ADDR_MASK	0xF0
#define MODE1_NORMAL_MODE	(0 << 4)
#define MODE1_SPEED_MODE	(1 << 4)

#define TLC591XX_REG_MODE2	0x01
#define MODE2_DIM		(0 << 5)
#define MODE2_BLINK		(1 << 5)
#define MODE2_OCH_STOP		(0 << 3)
#define MODE2_OCH_ACK		(1 << 3)

#define TLC591XX_REG_PWM(x)	(0x02 + (x))

#define TLC591XX_REG_GRPPWM	0x12
#define TLC591XX_REG_GRPFREQ	0x13
#define TLC591XX_REG_LEDOUT3	0x17

/* LED Driver Output State, determine the source that drives LED outputs */
#define LEDOUT_OFF		0x0	/* Output LOW */
#define LEDOUT_ON		0x1	/* Output HI-Z */
#define LEDOUT_DIM		0x2	/* Dimming */
#define LEDOUT_BLINK		0x3	/* Blinking */
#define LEDOUT_MASK		0x3

#define ldev_to_led(c)		container_of(c, struct tlc591xx_led, ldev)

static char *suspend_color_override = (char*)NULL;
module_param_named(suspend_color, suspend_color_override, charp, 0444);
MODULE_PARM_DESC(suspend_color, "suspend_color override");

static char *poweroff_state_override = (char*)NULL;
module_param_named(poweroff_state, poweroff_state_override, charp, 0444);
MODULE_PARM_DESC(poweroff_state, "poweroff_color override");

enum power_led_type {
	TYPE_RED = 12,
	TYPE_GREEN = 13,
	TYPE_BLUE = 14
};

struct tlc591xx_led {
	bool active;
	unsigned int led_no;
	struct led_classdev ldev;
	struct tlc591xx_priv *priv;
};

struct tlc591xx_priv {
	struct tlc591xx_led leds[TLC591XX_MAX_LEDS];
	struct regmap *regmap;
	unsigned int reg_ledout_offset;
};

struct tlc591xx {
	unsigned int max_leds;
	unsigned int reg_ledout_offset;
};

static const struct tlc591xx tlc59116 = {
	.max_leds = 16,
	.reg_ledout_offset = 0x14,
};

static const struct tlc591xx tlc59108 = {
	.max_leds = 8,
	.reg_ledout_offset = 0x0c,
};

static int
tlc591xx_set_mode(struct regmap *regmap, u8 mode)
{
	int err;
	u8 val;

	err = regmap_write(regmap, TLC591XX_REG_MODE1, MODE1_NORMAL_MODE);
	if (err)
		return err;

	val = MODE2_OCH_STOP | mode;

	return regmap_write(regmap, TLC591XX_REG_MODE2, val);
}

static int
tlc591xx_set_ledout(struct tlc591xx_priv *priv, struct tlc591xx_led *led,
		    u8 val)
{
	unsigned int i = (led->led_no % 4) * 2;
	unsigned int mask = LEDOUT_MASK << i;
	unsigned int addr = priv->reg_ledout_offset + (led->led_no >> 2);

	val = val << i;

	return regmap_update_bits(priv->regmap, addr, mask, val);
}

static int
tlc591xx_set_pwm(struct tlc591xx_priv *priv, struct tlc591xx_led *led,
		 u8 brightness)
{
	u8 pwm = TLC591XX_REG_PWM(led->led_no);

	return regmap_write(priv->regmap, pwm, brightness);
}

/* It's only for power LED */
static int
tlc591xx_set_blink_freq(struct tlc591xx_priv *priv, struct tlc591xx_led *led,
		 u8 color_value)
{
	regmap_write(priv->regmap, TLC591XX_REG_GRPPWM, 0x55);
	regmap_write(priv->regmap, TLC591XX_REG_GRPFREQ, 0x10);

	u8 pwm = TLC591XX_REG_PWM(led->led_no);
	regmap_write(priv->regmap, pwm, 60);

	return regmap_write(priv->regmap, TLC591XX_REG_LEDOUT3, color_value);
}

static int
tlc591xx_brightness_set(struct led_classdev *led_cdev,
			enum led_brightness brightness)
{
	struct tlc591xx_led *led = ldev_to_led(led_cdev);
	struct tlc591xx_priv *priv = led->priv;
	int err;

	switch ((int)brightness) {
	case 0:
		err = tlc591xx_set_ledout(priv, led, LEDOUT_OFF);
		break;
	case TLC591XX_MAX_BRIGHTNESS:
		err = tlc591xx_set_ledout(priv, led, LEDOUT_ON);
		break;
	default:
		err = tlc591xx_set_ledout(priv, led, LEDOUT_DIM);
		if (!err)
			err = tlc591xx_set_pwm(priv, led, brightness);
	}

	return err;
}

static const struct regmap_config tlc591xx_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x1e,
};

static const struct of_device_id of_tlc591xx_leds_match[] = {
	{ .compatible = "ti,tlc59116",
	  .data = &tlc59116 },
	{ .compatible = "ti,tlc59108",
	  .data = &tlc59108 },
	{},
};
MODULE_DEVICE_TABLE(of, of_tlc591xx_leds_match);

static int
tlc591xx_probe(struct i2c_client *client,
	       const struct i2c_device_id *id)
{
	struct device_node *np, *child;
	struct device *dev = &client->dev;
	const struct tlc591xx *tlc591xx;
	struct tlc591xx_priv *priv;
	int err, count, reg;

	np = dev_of_node(dev);
	if (!np)
		return -ENODEV;

	tlc591xx = device_get_match_data(dev);
	if (!tlc591xx)
		return -ENODEV;

	count = of_get_available_child_count(np);
	if (!count || count > tlc591xx->max_leds)
		return -EINVAL;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->regmap = devm_regmap_init_i2c(client, &tlc591xx_regmap);
	if (IS_ERR(priv->regmap)) {
		err = PTR_ERR(priv->regmap);
		dev_err(dev, "Failed to allocate register map: %d\n", err);
		return err;
	}
	priv->reg_ledout_offset = tlc591xx->reg_ledout_offset;

	i2c_set_clientdata(client, priv);

	err = tlc591xx_set_mode(priv->regmap, MODE2_BLINK);
	if (err < 0)
		return err;

	for_each_available_child_of_node(np, child) {
		struct tlc591xx_led *led;
		struct led_init_data init_data = {};

		init_data.fwnode = of_fwnode_handle(child);

		err = of_property_read_u32(child, "reg", &reg);
		if (err) {
			of_node_put(child);
			return err;
		}
		if (reg < 0 || reg >= tlc591xx->max_leds ||
		    priv->leds[reg].active) {
			of_node_put(child);
			return -EINVAL;
		}
		led = &priv->leds[reg];

		led->active = true;
		led->priv = priv;
		led->led_no = reg;
		led->ldev.brightness_set_blocking = tlc591xx_brightness_set;
		led->ldev.max_brightness = TLC591XX_MAX_BRIGHTNESS;
		err = devm_led_classdev_register_ext(dev, &led->ldev,
						     &init_data);
		if (err < 0) {
			of_node_put(child);
			return dev_err_probe(dev, err,
					     "couldn't register LED %s\n",
					     led->ldev.name);
		}

		tlc591xx_set_ledout(led->priv, led, LEDOUT_OFF);
	}
	return 0;
}

static void tlc591xx_shutdown(struct i2c_client *client)
{
	struct tlc591xx_priv *priv = i2c_get_clientdata(client);
	int i = TLC591XX_MAX_LEDS;

	while (--i >= 0) {
		if(i == TYPE_RED) {
			if(strcmp(poweroff_state_override, "on") == 0) {
				tlc591xx_set_ledout(priv, &priv->leds[i], LEDOUT_ON);
				tlc591xx_set_pwm(priv, &priv->leds[i], 20);
			} else
				tlc591xx_set_ledout(priv, &priv->leds[i], LEDOUT_OFF);
		}
		else
			tlc591xx_set_ledout(priv, &priv->leds[i], LEDOUT_OFF);
	}
}

static void tlc591xx_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct tlc591xx_priv *priv = i2c_get_clientdata(client);
	int i = TLC591XX_MAX_LEDS;
	int led_red_flag = 0, led_green_flag = 0, led_blue_flag = 1, blink_flag = 0;
	u8 current_color = 0x30;

	if(strcmp(suspend_color_override, "red") == 0) {
		led_red_flag = 1;
		led_green_flag = 0;
		led_blue_flag = 0;
		current_color = 0x03;
	}
	else if(strcmp(suspend_color_override, "green") == 0) {
		led_red_flag = 0;
		led_green_flag = 1;
		led_blue_flag = 0;
		current_color = 0x0c;
	}
	else if(strcmp(suspend_color_override, "blue") == 0) {
		led_red_flag = 0;
		led_green_flag = 0;
		led_blue_flag = 1;
		current_color = 0x30;
	}
	else if(strcmp(suspend_color_override, "yellow") == 0) {
		led_red_flag = 1;
		led_green_flag = 1;
		led_blue_flag = 0;
		current_color = 0x0f;
	}
	else if(strcmp(suspend_color_override, "magenta") == 0) {
		led_red_flag = 1;
		led_green_flag = 0;
		led_blue_flag = 1;
		current_color = 0x33;
	}
	else if(strcmp(suspend_color_override, "cyan") == 0) {
		led_red_flag = 0;
		led_green_flag = 1;
		led_blue_flag = 1;
		current_color = 0x3c;
	}
	else if(strcmp(suspend_color_override, "white") == 0) {
		led_red_flag = 1;
		led_green_flag = 1;
		led_blue_flag = 1;
		current_color = 0x3f;
	}
	else {
		led_red_flag = 0;
		led_green_flag = 0;
		led_blue_flag = 1;
		current_color = 0x03;
	}

	while (--i >= 0) {
		if(i == TYPE_RED && led_red_flag) {
			tlc591xx_set_ledout(priv, &priv->leds[i], LEDOUT_ON);
			if(!blink_flag)
				tlc591xx_set_blink_freq(priv, &priv->leds[i], current_color);
		} else if(i == TYPE_GREEN && led_green_flag) {
			tlc591xx_set_ledout(priv, &priv->leds[i], LEDOUT_ON);
			if(!blink_flag)
				tlc591xx_set_blink_freq(priv, &priv->leds[i], current_color);
		}  else if(i == TYPE_BLUE && led_blue_flag) {
			tlc591xx_set_ledout(priv, &priv->leds[i], LEDOUT_ON);
			if(!blink_flag)
				tlc591xx_set_blink_freq(priv, &priv->leds[i], current_color);
		} else
			tlc591xx_set_ledout(priv, &priv->leds[i], LEDOUT_OFF);
	}
}

static void tlc591xx_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct tlc591xx_priv *priv = i2c_get_clientdata(client);
	int i = TLC591XX_MAX_LEDS;

	while (--i >= 0) {
		tlc591xx_set_ledout(priv, &priv->leds[i], LEDOUT_OFF);
	}
}
static SIMPLE_DEV_PM_OPS(tlc591xx_pm_ops, tlc591xx_suspend, tlc591xx_resume);

static const struct i2c_device_id tlc591xx_id[] = {
	{ "tlc59116" },
	{ "tlc59108" },
	{},
};
MODULE_DEVICE_TABLE(i2c, tlc591xx_id);

static struct i2c_driver tlc591xx_driver = {
	.driver = {
		.name = "tlc591xx",
		.of_match_table = of_match_ptr(of_tlc591xx_leds_match),
		.pm = &tlc591xx_pm_ops,
	},
	.probe = tlc591xx_probe,
	.shutdown = tlc591xx_shutdown,
	.id_table = tlc591xx_id,
};

module_i2c_driver(tlc591xx_driver);

MODULE_AUTHOR("Andrew Lunn <andrew@lunn.ch>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TLC591XX LED driver");
