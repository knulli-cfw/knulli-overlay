/* Reads the battery level and charging state from sysfs.
 *
 * Only the writer side (the CLI) uses this: it stamps the reading into the
 * shared state, so the injected library never touches sysfs in a game process.
 */
#ifndef OV_BATTERY_H
#define OV_BATTERY_H

#define OV_BATTERY_UNKNOWN (-1)

/* Returns the charge in percent, or OV_BATTERY_UNKNOWN if there is no battery.
 * `*charging` is set to 1 while it is charging or full. */
int ov_battery_read(int *charging);

#endif /* OV_BATTERY_H */
