

#ifndef TARGET_NAME
#define TARGET_NAME CUDA
#endif
#ifndef CUDA_RTL_H
#define CUDA_RTL_H

#define GETNAME2(name) #name
#define GETNAME(name) GETNAME2(name)

#define DP(...) DEBUGP("Target " GETNAME(TARGET_NAME) " RTL", __VA_ARGS__)

#endif /* CUDA_RTL_H */

