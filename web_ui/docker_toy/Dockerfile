FROM ubuntu:14.04
SHELL ["/bin/bash", "-c"]

RUN \
  apt-get update && apt-get install -yq \
    git joe apache2 mysql-server php5 php5-dev php5-curl php-apc php5-mcrypt \
    php5-mysql php5-gd && \
  a2enmod rewrite && \
  php5enmod mcrypt

COPY 001-phabricator.conf /etc/apache2/sites-available/
 
WORKDIR '/home'

RUN git clone https://github.com/phacility/libphutil.git && \
  cd libphutil && \
  git checkout 0cd92b1ff5c4e3cabfe691c8a1794b23e1f3f720 && \
  cd .. && \
  git clone https://github.com/phacility/arcanist.git && \
  cd arcanist && \
  git checkout d9cb5b18fbc1a22630eeaa16da7d291b206fba21 && \
  cd .. && \
  git clone https://github.com/phacility/phabricator.git && \
  cd phabricator && \
  git checkout 577d4980339f68826d534110107687f33b176e88 && \
  cd ..

ADD web_ui.tgz /home/bistro/

# IMPORTANT: Setting an empty `bistro.get-state-hostname-suffix` below
# disables a security precaution built into the Bistro UI for the sake of
# letting you test with minimal hostname configuration.  Do NOT do this on
# production deployments!
RUN \
  php -i && \
  ln -s ../sites-available/001-phabricator.conf /etc/apache2/sites-enabled/ &&\
  rm /etc/apache2/sites-enabled/000-default.conf && \
  service mysql start && \
  /home/phabricator/bin/storage upgrade --force && \
  /home/bistro/plug_bistro_into_phabricator.sh /home && \
  /home/phabricator/bin/config set bistro.get-state-hostname-suffix '' && \
  service mysql stop
