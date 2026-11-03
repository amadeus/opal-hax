import math, struct
f32=lambda v: struct.unpack('<f',struct.pack('<f',v))[0]
pts=[(f32(0.0),f32(0.0)),(f32(120/255),f32(146/255)),(f32(1.0),f32(1.0))]
P=[(x*255.0,y*255.0) for x,y in pts]
n=len(P)
M=[[0.0]*3 for _ in range(n)]; R=[0.0]*n
M[0][1]=1
for i in range(1,n-1):
    P1,P2,P3=P[i-1],P[i],P[i+1]
    M[i][0]=(P2[0]-P1[0])/6; M[i][1]=(P3[0]-P1[0])/3; M[i][2]=(P3[0]-P2[0])/6
    R[i]=(P3[1]-P2[1])/(P3[0]-P2[0])-(P2[1]-P1[1])/(P2[0]-P1[0])
M[n-1][1]=1
for i in range(1,n):
    k=M[i][0]/M[i-1][1]; M[i][1]-=k*M[i-1][2]; M[i][0]=0; R[i]-=k*R[i-1]
for i in range(n-2,-1,-1):
    k=M[i][2]/M[i+1][1]; M[i][1]-=k*M[i+1][0]; M[i][2]=0; R[i]-=k*R[i+1]
sd=[R[i]/M[i][1] for i in range(n)]
out=[]
for i in range(n-1):
    cx,cy=P[i]; nx,ny=P[i+1]
    h=nx-cx
    for x in range(int(cx),int(nx)):
        t=(x-cx)/h; a=1-t; b=t
        y=a*cy+b*ny+(h*h/6)*((a*a*a-a)*sd[i]+(b*b*b-b)*sd[i+1])
        y=min(max(y,0.0),255.0)
        out.append((x,y))
out.append(P[-1])
curve=[0.0]*256
for x,y in out:
    d=f32(math.sqrt((x-x)**2+(x-y)**2))
    if x>y: d=-d
    curve[int(x)]=d
lut=[]
for x in range(256):
    i=int(min(max(float(f32(float(x)+0.0)),0.0),255.0))   # per-channel identity
    v=f32(float(i)+curve[i])
    lut.append(int(min(max(v,0.0),255.0)))
print('sd',sd)
print(lut)
