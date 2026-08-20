/* Where an element sits on the screen: one of the nine corner/edge/centre
 * anchors, packed into a byte so it fits in the shared state. */
#ifndef OV_ANCHOR_H_INCLUDED
#define OV_ANCHOR_H_INCLUDED

enum { OV_H_LEFT = 0, OV_H_CENTER = 1, OV_H_RIGHT = 2 };
enum { OV_V_TOP = 0, OV_V_MIDDLE = 1, OV_V_BOTTOM = 2 };

#define OV_ANCHOR(h, v)  (((v) << 2) | (h))
#define OV_ANCHOR_H(a)   ((a) & 3)
#define OV_ANCHOR_V(a)   (((a) >> 2) & 3)

#define OV_ANCHOR_DEFAULT_PANEL   OV_ANCHOR(OV_H_LEFT, OV_V_MIDDLE)
/* The clock has the top left corner, so the status pill moved across. */
#define OV_ANCHOR_DEFAULT_BATTERY OV_ANCHOR(OV_H_RIGHT, OV_V_TOP)
#define OV_ANCHOR_DEFAULT_CLOCK   OV_ANCHOR(OV_H_LEFT, OV_V_TOP)
#define OV_ANCHOR_DEFAULT_NOTIFICATION OV_ANCHOR(OV_H_CENTER, OV_V_BOTTOM)

/* Parses "top-right", "right top", "middle-right", "center", ... in either
 * order; a single axis leaves the other centred.  Returns `fallback` when the
 * string makes no sense. */
int ov_anchor_parse(const char *s, int fallback);

/* The other way round, for reporting what a setting resolved to: "top-left",
 * "bottom-center", ...  The string is static. */
const char *ov_anchor_name(int anchor);

#endif /* OV_ANCHOR_H_INCLUDED */
