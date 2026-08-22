
local sqrt,sin,cos,asin,atan=math.sqrt,math.sin,math.cos,math.asin,math.atan
-- Flat (n,m) index = n*(n+1)/2 + m + 1, so every table stays in Lua's array
-- part (no hash lookups in the inner loop).
local OFF={} ; for n=0,NMAX do OFF[n]=n*(n+1)//2 end
local NP=OFF[NMAX]+NMAX+1
-- one-time recursion constants (position independent)
local K,C,E={},{},{}
for n=1,NMAX do
  local k=sqrt((2*n-1)/(2*n)); if n==1 then k=k*sqrt(2) end
  K[n]=k
  for m=0,n-1 do
    local d=sqrt(n*n-m*m); local i=OFF[n]+m+1
    C[i]=(2*n-1)/d
    E[i]=(n>=m+2) and sqrt((n-1)*(n-1)-m*m)/d or 0
  end
end
local P,DP={},{}
for i=1,NP do P[i]=0; DP[i]=0 end
local CM,SM={},{}

-- lat,lon in degrees; year is a decimal year (e.g. 2027.6, default = EPOCH).
-- Returns (1) declination in degrees, EAST positive: true = magnetic + decl,
--         (2) horizontal field intensity H in nT.
-- H gives the caller WMM's own error bar for free:
--     sigma_D = sqrt(0.26^2 + (5417/H)^2)   degrees, 1-sigma
-- and the official reliability zones: H < 2000 nT is the WMM "Blackout Zone"
-- (a magnetic compass is unusable), 2000 <= H < 6000 nT the "Caution Zone".
function declination(lat,lon,year)
  local dt=(year or EPOCH)-EPOCH
  if lat>89.99 then lat=89.99 elseif lat<-89.99 then lat=-89.99 end
  local phi,lam=lat*0.017453292,lon*0.017453292
  local sp,cp=sin(phi),cos(phi)
  -- WGS-84 geodetic -> geocentric spherical
  local rc=6378.137/sqrt(1-0.006694380*sp*sp)
  local p,z=rc*cp,rc*0.993305620*sp
  local r=sqrt(p*p+z*z)
  local pp=asin(z/r)                 -- geocentric latitude
  local ct,st=sin(pp),cos(pp)        -- cos(colatitude), sin(colatitude)
  P[1],DP[1]=1,0                     -- (0,0)
  for n=1,NMAX do
    local o,o1=OFF[n],OFF[n-1]
    local k=K[n]
    local dnn=o1+n                   -- (n-1,n-1)
    P[o+n+1]=k*st*P[dnn]
    DP[o+n+1]=k*(st*DP[dnn]+ct*P[dnn])
    for m=0,n-1 do
      local i,j=o+m+1,o1+m+1
      local c,e=C[i],E[i]
      local pv=c*ct*P[j]
      local dv=c*(ct*DP[j]-st*P[j])
      if e~=0 then local h=OFF[n-2]+m+1; pv=pv-e*P[h]; dv=dv-e*DP[h] end
      P[i],DP[i]=pv,dv
    end
  end
  for m=0,NMAX do CM[m+1]=cos(m*lam); SM[m+1]=sin(m*lam) end
  local ratio=6371.2/r
  local X,Y,Z=0,0,0
  local pw=ratio*ratio
  local gi,hi=0,0
  for n=1,NMAX do
    pw=pw*ratio
    local o=OFF[n]
    local np1=n+1
    for m=0,n do
      gi=gi+1
      local i,m1=o+m+1,m+1
      local gv=G[gi]+dt*GD[gi]
      local cm,sm=CM[m1],SM[m1]
      local a
      if m>0 then
        hi=hi+1
        local hv=H[hi]+dt*HD[hi]
        a=gv*cm+hv*sm
        Y=Y+pw*m*(gv*sm-hv*cm)*P[i]
      else
        a=gv
      end
      X=X+pw*a*DP[i]
      Z=Z-np1*pw*a*P[i]
    end
  end
  Y=Y/st
  local d=pp-phi
  local Xg=X*cos(d)-Z*sin(d)
  return atan(Y,Xg)*57.29577951, sqrt(Xg*Xg+Y*Y)
end
