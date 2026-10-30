#ifndef FLOATLIB_H
#define FLOATLIB_H
#ifdef __cplusplus
extern "C" {
#endif
/* Hard-float add / multiply of two floats — a loadable ET_REL module. */
float float_add(float a, float b);
float float_mul(float a, float b);
/* a * b * app_gain(); app_gain is imported from main. */
float float_scale(float a, float b);
#ifdef __cplusplus
}
#endif
#endif
