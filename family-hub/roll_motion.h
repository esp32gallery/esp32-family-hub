#pragma once
#include "dice_sdf.h"
namespace rollmotion {
constexpr int RES = 240, STEPS = 4, COUNT = 6 * STEPS, SIDE = 480;
constexpr int ORDER[6] = {1, 2, 3, 6, 5, 4};
inline int anchor(int value) {
  for (int i=0;i<6;i++) if (ORDER[i]==value) return i*STEPS;
  return 0;
}
inline dice::Quat slerp(dice::Quat a, dice::Quat b, float t) {
  float d=a.w*b.w+a.x*b.x+a.y*b.y+a.z*b.z;
  if(d<0) { b={-b.w,-b.x,-b.y,-b.z}; d=-d; }
  if(d>0.9995f) return dice::qnorm({a.w+(b.w-a.w)*t,a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t});
  float th=acosf(fmaxf(-1,fminf(1,d))), s=sinf(th);
  float x=sinf((1-t)*th)/s,y=sinf(t*th)/s;
  return dice::qnorm({a.w*x+b.w*y,a.x*x+b.x*y,a.y*x+b.y*y,a.z*x+b.z*y});
}
inline dice::Quat pose(int frame) {
  frame=(frame%COUNT+COUNT)%COUNT;
  int segment=frame/STEPS;
  return slerp(dice::targetForValue(ORDER[segment],0.1f),
               dice::targetForValue(ORDER[(segment+1)%6],0.1f),float(frame%STEPS)/STEPS);
}
inline uint16_t avg(uint16_t a,uint16_t b) {
  return ((a&0xF7DE)>>1)+((b&0xF7DE)>>1);
}
// Exact full-frame stride and per-frame offset are shared with the host preview.
inline void compose(const uint16_t *cache, int frame, int lift, uint16_t *out) {
  const uint16_t *src=cache+size_t(frame)*RES*RES;
  for(int y=0;y<SIDE;y++) {
    int sourceY=y+lift;
    uint16_t *dst=out+size_t(y)*SIDE;
    if(sourceY<0||sourceY>=SIDE) { for(int x=0;x<SIDE;x++) dst[x]=0; continue; }
    int sy=sourceY/2, sy1=sy+1<RES?sy+1:sy;
    for(int x=0;x<SIDE;x++) {
      int sx=x/2,sx1=sx+1<RES?sx+1:sx;
      uint16_t top=(x&1)?avg(src[sy*RES+sx],src[sy*RES+sx1]):src[sy*RES+sx];
      uint16_t bot=(x&1)?avg(src[sy1*RES+sx],src[sy1*RES+sx1]):src[sy1*RES+sx];
      dst[x]=(sourceY&1)?avg(top,bot):top;
    }
  }
}
inline int distance(int from,int to) { return COUNT+(anchor(to)-anchor(from)+COUNT)%COUNT; }
inline int frameAt(int from,int to,float u) {
  float ease=1.0f-(1.0f-u)*(1.0f-u);
  return (anchor(from)+int(distance(from,to)*ease+0.5f))%COUNT;
}
inline int liftAt(float u) { return int(78*sinf(3.14159265f*u)); }
}
