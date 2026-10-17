# The bridge for a hosted deployment (Tools/Deploy/docker-compose.yml). The game files, the build output and the
# user-data overlay are mounted, not baked in.
FROM node:22-alpine
WORKDIR /srv/Tools/Bridge
COPY package.json package-lock.json ./
RUN npm ci --omit=dev
COPY *.mjs ./
COPY web ./web
USER node
EXPOSE 8080
CMD ["node", "bridge.mjs", "--host", "0.0.0.0", "--game", "/data/client", "--userdata", "/data/userdata", "--web", "/data/web", \
     "--allow", "127.0.0.1:3724=ac-authserver:3724,127.0.0.1:8085=ac-worldserver:8085"]
