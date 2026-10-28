{
  qname:       'core::Extensions',
  version:     1,
  // No `wire`: this is a meta-structural slot, not a data field.  It carries
  // a nested meta (the extension chain), not data-region bytes.
  layout:      'sub-meta',
  description: 'Extension hook.  A meta-structural slot carrying a nested meta (a "sub-meta"): the extension\'s atoms describe further bytes in the carrier\'s single data region.  It occupies no data-region bytes of its own.  The hook itself defines the base carrier identity, but its nested extension content is excluded from the base id hash.  Hops wrap: a new extension nests the existing chain.  See docs/spec/04-protocols.md §Extensions.',
  mutable_by:  'either',
  read_by:     'both',
  phase:       'both',
}
