---
layout: home
title: A fast, flexible toolkit for scheduling and running distributed tasks
id: home
hero: true
---

{% include content/gridblocks.html data_source=site.data.features grid_type="threeByGridBlockLeft" %}

## Why Use Bistro?

{% capture why_bistro %}{% include why-bistro.md %}{% endcapture %}
{{ why_bistro | markdownify }}

## Early-release software

Although Bistro has been in production at Facebook for over 3 years, the
present public release is partial, including just the server components. 
The CLI tools and web UI will be shipping shortly.

The good news is that the parts that are released are continuously verified
by build & unit tests on <a
href="https://travis-ci.org/facebook/bistro">Travis CI</a>, so they should
work out of the box.

There is already a lot of code, with which to play, so you have any thoughts
--- <a href="https://www.facebook.com/groups/bistro.scheduler/">write us a
note</a>!  Open-source projects thrive and grow on encouragement and
feedback.

## As flexible as a library

Although Bistro comes as a ready-to-deploy service, it may be helpful to
think of it as a library.  In our experience at Facebook, many teams'
distributed computing needs are highly individual, requiring more
customization over time.

Luckily, Bistro can adapt to your needs in three different ways.

**1. Configuration**: most aspects of a Bistro deployment can be tuned:

{::comment} XXX Linkify these headings once written -- 
 * [Configuration](docs/config-source/):
 * [Persistence](docs/persistence/):
 * [Code execution](docs/execution/):
 * [Data shards](docs/data-shards/):
{:/comment}

 * Configuration: 
   continuously re-read a file, or a more custom solution
 * Persistence: 
   use no database, SQLite, or something fancier
 * Code execution: 
   run alongside the scheduler, or on remote workers
 * Data shards: 
   manually configured, periodic, or script-generated

**2. Plugins**: Bistro's architecture is highly pluggable, so adding your
own code is a cinch:
 
 * Can't find the right scheduling policy? Write your own <a
   href="https://github.com/facebook/bistro/blob/master/bistro/scheduler/LongTailSchedulerPolicy.cpp">
   in about 50 lines of C++</a>.
 * <a href="https://github.com/facebook/bistro/blob/master/bistro/statuses/SQLiteTaskStore.h">
   Need a different DB</a>?
   <a href="https://github.com/facebook/bistro/blob/master/bistro/nodes/ScriptFetcher.h">
   Shard source</a>?
   <a href="https://github.com/facebook/bistro/blob/master/bistro/config/FileConfigLoader.h">
   Configuration source</a>?

Every plugin you <a
href="https://github.com/facebook/bistro/blob/master/CONTRIBUTING.md">
contribute back to the community</a> makes the next person's customization
easier.

**3. Embedding**: Or, link just the pieces you want into your custom
application.  Need a <a
href="https://github.com/facebook/bistro/tree/master/bistro/cron"> C++ cron
library</a>?  An optionally-persistent <a
href="https://github.com/facebook/bistro/tree/master/bistro/statuses">task
status store</a>?

## Getting help, and helping out

### Questions and bug reports

To <a href="support.html">contact the maintainers</a>, post in <a
href="https://www.facebook.com/groups/bistro.scheduler">this Facebook
Group</a> or <a href="https://github.com/facebook/bistro/issues/new">file a
Github Issue</a>.

{% capture help_tips %}{% include help-tips.md %}{% endcapture %}
{{ help_tips | markdownify }}

### Contributions

We gladly welcome contributions to both code and documentation --- whether
you refactor a whole module, or fix one misspelling.  Please send <a
href="https://github.com/facebook/bistro/compare/">pull requests</a> for
code against the <a
href="https://github.com/facebook/bistro/tree/master">master branch</a> and
for the website or documentation against the <a
href="https://github.com/facebook/bistro/tree/gh-pages">gh-pages branch</a>.

Our <a
href="https://github.com/facebook/bistro/blob/master/CONTRIBUTING.md">
CONTRIBUTING.md</a> aims to expedite the acceptance of your pull request. 
It includes a 15-line C++ style guide, and explains Facebook's streamlined
<a href="https://code.facebook.com/cla">contributor license agreement</a>.

## Case study: data-parallel job with data **and** worker resources

{% capture case_study %}{% include case-study-db.md %}{% endcapture %}
{{ case_study | markdownify }}

## License

Bistro is
[BSD-licensed](https://github.com/facebook/bistro/blob/master/LICENSE).  We
also provide an [additional patent
grant](https://github.com/facebook/bistro/blob/master/PATENTS).
