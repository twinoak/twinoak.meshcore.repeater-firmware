// Taking a pointer to a **private** member of a class nobody here may edit, in the one way the
// C++ standard sanctions: `[temp.explicit]` - "the usual access checking rules do not apply to
// names used to specify explicit instantiations". No cast, no `#define private public`, no
// `offsetof` arithmetic, no copy of the class's layout. The member's own name appears exactly
// once, as a template argument of an explicit instantiation, and the pointer comes back out of a
// friend function that names nothing private at all.
//
// **Why this exists at all.** `spec/SYSTEM.md` S-2 says not one file of MeshCore's tree is
// edited, and its *exception* of 7 November 2026 says what this is for: link 1.2's
// `LOCAL_REQUEST` (`spec/LINK-PROTOCOL.md` 5.9) must be answered by upstream's own
// `MyMesh::handleRequest` out of upstream's own `reply_data`, and both are private at the pin
// (`examples/simple_repeater/MyMesh.h`: the class body begins with no access specifier, so
// everything before its `protected:` is private). Re-implementing the four answers was the
// alternative and was refused: the neighbour table and the ACL are private too, and a copy of
// four reply layouts would drift at every pin and be noticed on the air, not in a build.
// `src/roles/repeater/link/LocalRequest.cpp` is the **one** file that names either member, and
// `docs/upstream.md` lists both as a place a bump re-reads. The request to upstream that would
// make this unnecessary - one word, `protected:` moved up a line - is in
// `tasks/phase-3.9/reports/02.md`.
//
// **How it works**, in three moves:
//
//   1. A *tag* type is declared for each member, deriving from `PrivateMemberOf<Tag, Member>`.
//      That base declares `privateMemberOf(Tag)`, a friend of the base and therefore a member of
//      this namespace - and, because the base is a base class of the tag, a friend declared in an
//      **associated class** of the tag, which is what makes argument-dependent lookup find it
//      from anywhere ([basic.lookup.argdep]). Nothing is defined yet; a call here would not link.
//   2. `template struct TakePrivateMember<Tag, &Thing::member>;` - an explicit instantiation, the
//      one context where `&Thing::member` may name a private member. Instantiating it defines
//      `privateMemberOf(Tag)`, whose body is the template argument and nothing else.
//   3. `privateMemberOf(Tag())` hands back the pointer. Its type is the tag's `Type`, so the
//      call site is checked by the compiler and a pin that changes the member's type or signature
//      is a build error on the bump's own pull request.
//
// The tag and the explicit instantiation must be in **this** namespace, which is why
// `LocalRequest.cpp` reopens it: the declaration in move 1 and the definition in move 2 are
// members of the innermost namespace enclosing the class each appears in ([namespace.memdef]),
// and they have to be one function and not two.
//
// Host-portable: no Arduino, no MeshCore, nothing but the language. `test/test_private_member`
// holds the whole idiom against a stand-in class with a private function and a private array, in
// the native environment, so that the mechanism is tested where no board is needed and only its
// two uses need a board.

#pragma once

// GCC warns about the one unusual thing in the two templates below: `-Wnon-template-friend`, "a
// friend declaration declares a non-template function". It is on by default and this repository
// builds with -Werror, so it is silenced here and nowhere else. The warning's own advice - make
// it a function template and write `privateMemberOf<Tag>` - would not do: the definition would
// then have to be an explicit specialisation inside a class template, which is a harder thing to
// read for no gain. A friend that is not a template is exactly what is wanted, and the two
// declarations below are the whole of it. Clang does not know the option's name, so the pragma is
// GCC's alone.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-template-friend"
#endif

namespace twinoak {
namespace cxx {

// Move 1: the base every tag derives from. `Member` is the pointer-to-member type - for a
// function `int (C::*)(int)`, for a data member `int C::*`, for an array member
// `uint8_t (C::*)[N]`.
template <class Tag, class Member>
struct PrivateMemberOf {
  typedef Member Type;

  // Declared and not defined. `Tag` is still incomplete here (it is deriving from this), which a
  // function parameter type is allowed to be.
  friend Member privateMemberOf(Tag);
};

// Move 2: the explicit instantiation of this is what defines the function above. `kMember` is
// the pointer, and the body of the definition is that template argument - so no name of the
// class whose member it is appears in this file at all.
template <class Tag, typename Tag::Type kMember>
struct TakePrivateMember {
  friend typename Tag::Type privateMemberOf(Tag) { return kMember; }
};

}  // namespace cxx
}  // namespace twinoak

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
