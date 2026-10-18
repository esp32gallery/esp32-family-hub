#include <cstdio>
#include <vector>
#include "../family-hub/roll_motion.h"
static void ppm(const char *name,const uint16_t *buf) {
  FILE *f=fopen(name,"wb"); if(!f) { perror(name); exit(1); }
  fprintf(f,"P6\n480 480\n255\n");
  for(int i=0;i<480*480;i++) {
    uint16_t c=buf[i]; unsigned char rgb[3]={(unsigned char)(((c>>11)&31)*255/31),(unsigned char)(((c>>5)&63)*255/63),(unsigned char)((c&31)*255/31)};
    fwrite(rgb,1,3,f);
  }
  fclose(f);
}
int main() {
  std::vector<uint16_t> cache(rollmotion::COUNT*rollmotion::RES*rollmotion::RES),out(480*480);
  dice::Renderer r; r.W=r.H=rollmotion::RES; r.p.focal=800;
  for(int i=0;i<rollmotion::COUNT;i++) {
    r.buf=cache.data()+size_t(i)*rollmotion::RES*rollmotion::RES;
    r.render(rollmotion::pose(i),0);
  }
  for(int from=1;from<=6;from++) for(int to=1;to<=6;to++) for(int i=0;i<=12;i++) {
    float u=i/12.0f;
    rollmotion::compose(cache.data(),rollmotion::frameAt(from,to,u),rollmotion::liftAt(u),out.data());
    if(i==12) { r.W=r.H=480; r.buf=out.data(); memset(r.buf,0,480*480*2);r.render(dice::targetForValue(to,.1f),0); r.W=r.H=rollmotion::RES; }
    char file[128]; snprintf(file,sizeof(file),"preview/frames/%d-%d-%02d.ppm",from,to,i); ppm(file,out.data());
  }
}
