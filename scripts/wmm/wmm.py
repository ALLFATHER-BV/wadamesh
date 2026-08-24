import math

A_WGS84 = 6378.137
F_WGS84 = 1.0/298.257223563
B_WGS84 = A_WGS84*(1.0-F_WGS84)
E2 = F_WGS84*(2.0-F_WGS84)
RE = 6371.2   # geomagnetic reference radius, km

def load_cof(path):
    g={}; h={}; gd={}; hd={}; epoch=None
    with open(path) as f:
        for line in f:
            t=line.split()
            if len(t)==3:
                epoch=float(t[0]); continue
            if len(t)!=6: continue
            n=int(t[0]); m=int(t[1])
            g[(n,m)]=float(t[2]); h[(n,m)]=float(t[3])
            gd[(n,m)]=float(t[4]); hd[(n,m)]=float(t[5])
    N=max(k[0] for k in g)
    return epoch,N,g,h,gd,hd

def schmidt(N, theta):
    """Schmidt semi-normalized P[n][m](cos theta) and dP/dtheta."""
    ct=math.cos(theta); st=math.sin(theta)
    P=[[0.0]*(N+1) for _ in range(N+1)]
    dP=[[0.0]*(N+1) for _ in range(N+1)]
    P[0][0]=1.0; dP[0][0]=0.0
    for n in range(1,N+1):
        # sectorial
        k=math.sqrt((2*n-1)/(2.0*n))
        if n==1: k*=math.sqrt(2.0)   # Schmidt m=0 -> m=1 boundary
        P[n][n]=k*st*P[n-1][n-1]
        dP[n][n]=k*(st*dP[n-1][n-1]+ct*P[n-1][n-1])
        for m in range(0,n):
            d=math.sqrt(n*n-m*m)
            c=(2*n-1)/d
            P[n][m]=c*ct*P[n-1][m]
            dP[n][m]=c*(ct*dP[n-1][m]-st*P[n-1][m])
            if n-2>=m:
                e=math.sqrt((n-1)**2-m*m)/d
                P[n][m]-=e*P[n-2][m]
                dP[n][m]-=e*dP[n-2][m]
    return P,dP

def wmm(model, lat, lon, h_km, year):
    epoch,N,gc,hc,gd,hd = model
    dt = year-epoch
    phi=math.radians(lat); lam=math.radians(lon)
    sp=math.sin(phi); cp=math.cos(phi)
    Rc=A_WGS84/math.sqrt(1.0-E2*sp*sp)
    p=(Rc+h_km)*cp
    z=(Rc*(1.0-E2)+h_km)*sp
    r=math.sqrt(p*p+z*z)
    phip=math.asin(z/r)          # geocentric latitude
    theta=math.pi/2.0-phip
    P,dP=schmidt(N,theta)
    cml=[math.cos(m*lam) for m in range(N+1)]
    sml=[math.sin(m*lam) for m in range(N+1)]
    Xp=Yp=Zp=0.0
    ratio=RE/r
    for n in range(1,N+1):
        f=ratio**(n+2)
        for m in range(0,n+1):
            g=gc[(n,m)]+dt*gd[(n,m)]
            hh=hc[(n,m)]+dt*hd[(n,m)]
            a=g*cml[m]+hh*sml[m]
            b=g*sml[m]-hh*cml[m]
            Xp += f*a*dP[n][m]          # note: dP/dphi' = -dP/dtheta -> handled by sign below
            Yp += f*m*b*P[n][m]
            Zp += -(n+1)*f*a*P[n][m]
    pass
    cpp=math.cos(phip)
    Yp = Yp/cpp
    d=phip-phi
    X=Xp*math.cos(d)-Zp*math.sin(d)
    Z=Xp*math.sin(d)+Zp*math.cos(d)
    Y=Yp
    H=math.hypot(X,Y)
    F=math.sqrt(H*H+Z*Z)
    D=math.degrees(math.atan2(Y,X))
    I=math.degrees(math.atan2(Z,H))
    return dict(X=X,Y=Y,Z=Z,H=H,F=F,D=D,I=I)
