---
id: why-bistro
title: Why use Bistro?
layout: docs
permalink: /docs/why-bistro/
---

Bistro is a toolkit for making services that schedule and execute tasks.  It
is an engineer's tool --- your clients need to do large amounts of
computation, and your goal is to make a system that handles them easily,
perfomantly, reliably.

{% capture why_bistro %}{% include why-bistro.md %}{% endcapture %}
{{ why_bistro | markdownify }}
