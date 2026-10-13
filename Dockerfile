# Production image: compiled once, runs as a tiny Alpine container.
# Built in Azure by deploy/bin/deploy (az acr build); can also be built locally:
#   docker build -t kotoa-kasin .

FROM alpine:3.20 AS build
RUN apk add --no-cache build-base cmake sqlite-dev
WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY tests ./tests
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j \
 && ctest --test-dir build --output-on-failure
# The bundles served from /js and /css are built from js/ and css/ (see their bundles.txt).
COPY js ./js
COPY css ./css
RUN build/kasin-jsbundle -o bundles/js js/bundles.txt \
 && build/kasin-cssbundle -o bundles/css css/bundles.txt

FROM alpine:3.20
RUN apk add --no-cache sqlite-libs \
 && mkdir /data
COPY --from=build /src/build/kasin-httpd /usr/local/bin/kasin-httpd
COPY public /srv/public
COPY --from=build /src/bundles/ /srv/public/
COPY migrations /srv/migrations
EXPOSE 8080
# Two workers fit the smallest Container Apps size (0.25 vCPU, 0.5 GiB) with room
# to spare. /data is an Azure Files share; SQLite cannot take POSIX file locks there, so
# it locks with a ".lock" directory and keeps a rollback journal (-s share).
CMD ["kasin-httpd", "-p", "8080", "-t", "2", "-r", "/srv/public", "-d", "/data/kotoa.db", "-m", "/srv/migrations", "-u", "/data/uploads", "-s", "share"]
