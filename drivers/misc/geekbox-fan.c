/*
 * gpio-fan.c - driver for fans controlled by GPIO.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>
#include <linux/time.h>
#include <linux/regulator/consumer.h>
#include <linux/rockchip/common.h>
#include <linux/rockchip/dvfs.h>
#include <linux/workqueue.h>

#define GBOX_FAN_TRIG_TEMP		50	// 50 degree if not set
#define GBOX_FAN_LOOP_SECS 		30 * HZ	// 30 seconds
#define GBOX_FAN_LOOP_NODELAY_SECS      0
#define GBOX_FAN_GPIO_OFF		0
#define GBOX_FAN_GPIO_ON		1

enum gbox_fan_mode {
	GBOX_FAN_STATE_OFF = 0,
	GBOX_FAN_STATE_ON,
	GBOX_FAN_STATE_AUTO,
};

struct gbox_fan_data {
	struct platform_device *pdev;
	struct regulator *regulator;
	struct class *class;
	struct delayed_work work;
	enum gbox_fan_mode mode;
	int	ctrl_gpio;
	int	trig_temp;
};

static void fan_work_func(struct work_struct *_work)
{
	int temp = INVALID_TEMP;
	int volt;
	struct gbox_fan_data *fan_data = container_of(_work,
		   struct gbox_fan_data, work.work);

	volt = regulator_get_voltage(fan_data->regulator);
	temp = rockchip_tsadc_get_temp(0, volt);
	//printk("__Fan: temp[%d], mode[%d].\n", temp, fan_data->mode);
	if (temp > fan_data->trig_temp && temp != INVALID_TEMP)
		gpio_set_value(fan_data->ctrl_gpio, GBOX_FAN_GPIO_ON);
	else
		gpio_set_value(fan_data->ctrl_gpio, GBOX_FAN_GPIO_OFF);

	schedule_delayed_work(&fan_data->work, GBOX_FAN_LOOP_SECS);
}

static void gbox_fan_mode_set(struct gbox_fan_data  *fan_data)
{
	switch (fan_data->mode) {
	case GBOX_FAN_STATE_OFF:
		cancel_delayed_work(&fan_data->work);
		gpio_set_value(fan_data->ctrl_gpio, GBOX_FAN_GPIO_OFF);
		break;

	case GBOX_FAN_STATE_ON:
		cancel_delayed_work(&fan_data->work);
		gpio_set_value(fan_data->ctrl_gpio, GBOX_FAN_GPIO_ON);
		break;

	case GBOX_FAN_STATE_AUTO:
		// FIXME: achieve with a better way
		schedule_delayed_work(&fan_data->work, GBOX_FAN_LOOP_NODELAY_SECS);
		break;

	default:
		break;
	}
}

static ssize_t fan_mode_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct gbox_fan_data *fan_data = dev_get_drvdata(dev);

	return sprintf(buf, "Fan mode: %d\n", fan_data->mode);
}

static ssize_t fan_mode_store(struct device *dev, struct device_attribute *attr,
		       const char *buf, size_t count)
{
	struct gbox_fan_data *fan_data = dev_get_drvdata(dev);
	int mode;

	if (kstrtoint(buf, 3, &mode))
		return -EINVAL;

	fan_data->mode = mode;
	gbox_fan_mode_set(fan_data);

	return count;
}

static struct device_attribute fan_class_attrs[] = {
	__ATTR(mode, S_IRUGO | S_IWUGO, fan_mode_show, fan_mode_store),
	__ATTR_NULL,
};

static int gbox_fan_probe(struct platform_device *pdev)
{
	struct gbox_fan_data *fan_data = pdev->dev.platform_data;
	struct device *dev = &pdev->dev;
	struct class *fclass;
	int ret;

	fan_data = devm_kzalloc(dev, sizeof(struct gbox_fan_data), GFP_KERNEL);
	if (!fan_data)
		return -ENOMEM;

	ret = of_property_read_u32(dev->of_node, "trig-temp", &fan_data->trig_temp);
	if (ret < 0)
		fan_data->trig_temp = GBOX_FAN_TRIG_TEMP;

	fan_data->ctrl_gpio = of_get_named_gpio(dev->of_node, "ctrl-gpio", 0);
	if (gpio_request(fan_data->ctrl_gpio, "FAN") != 0)
		return -EIO;

	gpio_direction_output(fan_data->ctrl_gpio, GBOX_FAN_GPIO_OFF);
	fan_data->mode = GBOX_FAN_STATE_OFF;

	fan_data->regulator = regulator_get(dev, "vdd_arm");

	INIT_DELAYED_WORK(&fan_data->work, fan_work_func);

	fan_data->pdev = pdev;
	platform_set_drvdata(pdev, fan_data);

	fclass = fan_data->class;
	fclass = class_create(THIS_MODULE, "fan");
	if (IS_ERR(fclass))
		return PTR_ERR(fclass);
	fclass->dev_attrs = fan_class_attrs;
	device_create(fclass, dev->parent, 0, fan_data, "ctrl");

	dev_info(dev, "trigger temperature is %d.\n", fan_data->trig_temp);

	return 0;
}

static int gbox_fan_remove(struct platform_device *pdev)
{
	struct gbox_fan_data *fan_data = platform_get_drvdata(pdev);

	fan_data->mode = GBOX_FAN_STATE_OFF;
	gbox_fan_mode_set(fan_data);

	regulator_put(fan_data->regulator);

	return 0;
}

static void gbox_fan_shutdown(struct platform_device *pdev)
{
	struct gbox_fan_data *fan_data = platform_get_drvdata(pdev);

	fan_data->mode = GBOX_FAN_STATE_OFF;
	gbox_fan_mode_set(fan_data);
}

#ifdef CONFIG_OF_GPIO
static struct of_device_id of_gbox_fan_match[] = {
	{ .compatible = "gbox-fan", },
	{},
};
#endif

static struct platform_driver gbox_fan_driver = {
	.probe	= gbox_fan_probe,
	.remove	= gbox_fan_remove,
	.shutdown = gbox_fan_shutdown,
	.driver	= {
		.name	= "gbox-fan",
		#ifdef CONFIG_OF_GPIO
		.of_match_table = of_match_ptr(of_gbox_fan_match),
		#endif
	},
};

module_platform_driver(gbox_fan_driver);

MODULE_AUTHOR("Gouwa <gouwa@szwesion.com>");
MODULE_DESCRIPTION("Geekbox GPIO Fan driver");
MODULE_LICENSE("GPL");
