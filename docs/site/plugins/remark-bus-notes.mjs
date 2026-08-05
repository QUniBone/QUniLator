/**
 * A passage that applies to one bus only, written as a labelled blockquote.
 *
 *   > **QBUS · QBone**
 *   >
 *   > QBUS runs 16, 18 or 22 bits, and the processor decides which.
 *
 * The label is the whole of the blockquote's first paragraph. GitHub and any
 * Markdown viewer render the block as an ordinary quotation; here it becomes the
 * same `<aside class="bus-note">` the site styles, so the manual reads correctly
 * whether it is browsed in git or on the web.
 *
 * The block is always rendered and always labelled, whatever the reader has
 * selected: a deep link must never hide content, and a QBone operator is better
 * off knowing that UNIBUS differs here than seeing a gap. Selecting a board
 * de-emphasises the blocks that do not apply; it never removes them.
 */
import { visit } from 'unist-util-visit';
import { toString } from 'mdast-util-to-string';

/** The label that marks a blockquote as bus-specific, and the bus it names. */
const LABELS = new Map([
	['UNIBUS · UniBone', 'unibus'],
	['QBUS · QBone', 'qbus'],
]);

/** An mdast node that renders as an arbitrary HTML element, children and all. */
function element(tagName, properties, children) {
	return { type: 'paragraph', data: { hName: tagName, hProperties: properties }, children };
}

export function remarkBusNotes() {
	return () => (tree) => {
		visit(tree, 'blockquote', (node, index, parent) => {
			if (!parent || index === undefined) return;

			const [first, ...body] = node.children;
			if (first?.type !== 'paragraph') return;

			// The label paragraph is nothing but the bolded label: a quotation that
			// merely opens with bold text is left as a quotation.
			const [strong] = first.children;
			if (first.children.length !== 1 || strong?.type !== 'strong') return;

			const label = toString(strong);
			const bus = LABELS.get(label);
			if (!bus) return;

			parent.children[index] = element(
				'aside',
				{ class: 'bus-note', 'data-bus': bus, 'aria-label': `Applies to ${label} only` },
				[
					element('p', { class: 'bus-note__label' }, [{ type: 'text', value: label }]),
					element('div', { class: 'bus-note__body' }, body),
				]
			);
		});
	};
}
